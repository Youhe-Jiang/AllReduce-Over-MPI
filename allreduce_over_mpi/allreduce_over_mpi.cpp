//start of flextree mod
#ifndef FlexTree_MPI
#define FlexTree_MPI

#if defined(c_plusplus) || defined(__cplusplus)
#include<iostream>

static int FT_enabled()
{
    std::cout << "FlexTree enabled";
    return 0;
}

#include<sstream>
#include<fstream>
#include<vector>
#include<string.h>
#include<thread>
#ifndef OMPI_MPI_H
#include<mpi.h>
const int INF = 0x3F3F3F3F;
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

//typedef float DataType;


//#define SHOW_TIME // 显示更多的时间调试信息
#ifdef SHOW_TIME
double _time_base;
#define TIME_RESET() do {_time_base=MPI_Wtime();} while (false)
#define TIME_LOG_IF(exp, note) do {LOG_IF(INFO,exp)<<MPI_Wtime()-_time_base<<" :: "<<note;} while (false)
#endif

// Op
class Operation
{
public:    
    size_t peer;
    std::vector<size_t> blocks;
    /**
     * Operation 类构造函数. 用于 Tree AllReduce.
     *  
     * @param _peer 接受/发送操作的对象
     * @param _total_peers 参与计算的总节点数
     * @param _gap 接收/发送的数据块的编号间距
     */
    Operation(size_t _peer, size_t _total_peers, size_t _gap): peer(_peer)
    {
        size_t start = _peer % _gap;
        for (size_t i = start; i < _total_peers; i += _gap)
        {
            blocks.push_back(i);
        }
    }
    /**
     * Operation 类构造函数.
     *  
     * @param _peer 接受/发送操作的对象
     * @param _block 接收/发送的数据块的编号
     */
    Operation(size_t _peer, size_t _block): peer(_peer)
    {
        blocks.push_back(_block);
    }
};

// lonely 的意思是: 被树孤立的. 在 ar 过程中, lonely 节点的数据会不按照 stages 来进行, 而是与树的 ar 过程同步并行.
class Operations
{
public:
    std::vector<size_t> stages;
    size_t total_peers, node_label, num_lonely, num_split;
public:
    std::vector<std::vector<Operation>> ops;
    std::vector<Operation> lonely_ops;
    /**
     * Operations 类的构造函数
     * 
     * @param _total_peers 参与计算的总节点数
     * @param _node_label 当前节点的编号
     * @param _stages 一个向量, 记录了 AllReduce 树自下而上每一层的宽度. 注意积 + {@code _num_lonely} 应当等于 {@code _total_peers}.
     * @param _num_lonely 孤立节点的数量
     */ 
    Operations(size_t _total_peers, size_t _num_lonely, size_t _node_label, std::vector<size_t> _stages): total_peers(_total_peers), node_label(_node_label), stages(_stages), num_lonely(_num_lonely), num_split(_total_peers - _num_lonely)
    {

        size_t pi = 1;
        for (const auto &i:_stages)
        {
            pi *= i;
        }
    }
    // 生成拓扑, 要求子类实现
    virtual void generate_ops() = 0;
    // 打印拓扑
    virtual void print_ops()const
    {
        std::cout << typeid(*this).name() << " of node " << node_label << " in total " << total_peers << " peers: " << std::endl;
        for (const auto &i:ops)
        {
            if (&i != &*(ops.end() - 1))
            {
                std::cout << "┝ stage";
            }
            else 
            {
                std::cout << "┕ stage";
            }
            for (const auto &j:i)
            {
                std::cout<< " | node " << j.peer<<": ";
                for (auto k:j.blocks)
                {
                    std::cout<<k<<",";
                }
            }
            std::cout<<std::endl;
        }
        if (num_lonely != 0)
        {
            std::cout << "and " << num_lonely << " lonely node(s):" << std::endl;
            std::cout << "┕ lonely";
            for (const auto &j:lonely_ops)
            {
                std::cout<< " | node " << j.peer<<": ";
                for (auto k:j.blocks)
                {
                    std::cout<<k<<",";
                }
            }
            std::cout<<std::endl;
        }
    }
};

class Send_Ops: public Operations
{
public:
    using Operations::Operations;
    // 生成逻辑拓扑
    virtual void generate_ops()
    {
        if (node_label < num_split)
        {
            // 当前组内成员的编号的间距
            size_t gap = 1;
            for (auto i:stages)
            {
                std::vector<Operation> stage_ops;
                // 当前组内编号最小的成员
                size_t left_peer = node_label / (gap * i) * (gap * i) + node_label % gap;
                for (size_t j = 0; j < i; j++)
                {
                    stage_ops.emplace_back(left_peer, num_split, gap * i);
                    left_peer += gap;
                }
                ops.push_back(stage_ops);
                gap *= i;
            }
        }
        else 
        {
            for (size_t i = 0; i < num_split; i++)
            {
                lonely_ops.emplace_back(i, i);
            }
        }
    }
};

class Recv_Ops: public Operations
{
public:
    using Operations::Operations;
    // 生成逻辑拓扑
    virtual void generate_ops()
    {
        if (node_label < num_split)
        {
            // 当前组内成员的编号的间距
            size_t gap = 1;
            for (auto i:stages)
            {
                std::vector<Operation> stage_ops;
                Operation op_template(node_label, num_split, gap * i);
                // 当前组内编号最小的成员
                size_t left_peer = node_label / (gap * i) * (gap * i) + node_label % gap;
                for (size_t j = 0; j < i; j++)
                {
                    op_template.peer = left_peer;
                    stage_ops.emplace_back(op_template);
                    left_peer += gap;
                }
                ops.push_back(stage_ops);
                gap *= i;
            }
            for (size_t i = num_split; i < total_peers; i++)
            {
                lonely_ops.emplace_back(i, node_label);
            }
        }
    }
};

class FlexTree_Context
{
public:
    size_t num_nodes, node_label, num_lonely, data_size, num_split, split_size, data_size_aligned, type_size, last_split_size;
    bool has_lonely;
    FlexTree_Context(MPI_Comm _comm, MPI_Datatype _datatype, size_t _count, size_t _num_lonely = 0)
    {
        int tmp;
        MPI_Comm_size(_comm, &tmp);
        num_nodes = tmp;
        MPI_Comm_rank(_comm, &tmp);
        node_label = tmp;
        num_lonely = _num_lonely;
        data_size = _count;
        num_split = num_nodes - num_lonely;
        split_size = (data_size + num_nodes - 1) / num_nodes; // aligned
        data_size_aligned = split_size * num_nodes;
        MPI_Type_size(_datatype, &tmp);
        type_size = tmp;
        last_split_size = split_size - (data_size_aligned - data_size);
        has_lonely = (num_lonely > 0);
    }
    void show_context()
    {
        std::cout << "num_nodes=" << num_nodes << ", node_label=" << node_label << ", num_lonely=" << num_lonely << ", data_size=" << data_size << ", num_split=" << num_split << ", split_size=" << split_size << ", data_size_aligned=" << data_size_aligned << ", type_size=" << type_size << ", has_lonely=" << has_lonely << std::endl;
    }
};

const size_t MAX_NUM_BLOCKS = 20;
template<class DataType> 
static void reduce_sum(DataType **src, DataType *dst, int num_blocks, size_t num_elements)
{
    //std::cout << "reduce_sum called, ele size = " << sizeof(**src) << std::endl;
    if (num_blocks <= 1) return;
#define PARALLEL_THREAD 14
    DataType *src0 = src[0];
    DataType *src1 = src[1];
    DataType *src2 = src[2];
    DataType *src3 = src[3];
    DataType *src4 = src[4];
    DataType *src5 = src[5];
    DataType *src6 = src[6];
    DataType *src7 = src[7];
    DataType *src8 = src[8];
    DataType *src9 = src[9];
    DataType *src10 = src[10];
    DataType *src11 = src[11];
    DataType *src12 = src[12];
    DataType *src13 = src[13];
    DataType *src14 = src[14];
    DataType *src15 = src[15];
    DataType *src16 = src[16];
    DataType *src17 = src[17];
    DataType *src18 = src[18];
    DataType *src19 = src[19];

    switch (num_blocks)
    {
    case 2:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i];
        }
        break;
    }
    case 3:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i];
        }
        break;
    }
    case 4:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i];
        }
        break;
    }
    case 5:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i];
        }
        break;
    }
    case 6:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i];
        }
        break;
    }
    case 7:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i];
        }
        break;
    }
    case 8:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i];
        }
        break;
    }
    case 9:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i];
        }
        break;
    }
    case 10:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i];
        }
        break;
    }
    case 11:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i];
        }
        break;
    }
    case 12:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i];
        }
        break;
    }
    case 13:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i];
        }
        break;
    }
    case 14:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i];
        }
        break;
    }
    case 15:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i] + src14[i];
        }
        break;
    }
    case 16:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i] + src14[i] + src15[i];
        }
        break;
    }
    case 17:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i] + src14[i] + src15[i] + src16[i];
        }
        break;
    }
    case 18:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i] + src14[i] + src15[i] + src16[i] + src17[i];
        }
        break;
    }
    case 19:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i] + src14[i] + src15[i] + src16[i] + src17[i] + src18[i];
        }
        break;
    }
    case 20:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] + src1[i] + src2[i] + src3[i] + src4[i] + src5[i] + src6[i] + src7[i] + src8[i] + src9[i] + src10[i] + src11[i] + src12[i] + src13[i] + src14[i] + src15[i] + src16[i] + src17[i] + src18[i] + src19[i];
        }
        break;
    }
    default:
        std::cerr << "Unknown num_blocks: " << num_blocks << std::endl;
        break;
    }
}

template<class DataType> 
static void reduce_band(DataType **src, DataType *dst, int num_blocks, size_t num_elements)
{
    //std::cout << "reduce_band called, ele size = " << sizeof(**src) << std::endl;
    if (num_blocks <= 1) return;
#define PARALLEL_THREAD 14
    DataType *src0 = src[0];
    DataType *src1 = src[1];
    DataType *src2 = src[2];
    DataType *src3 = src[3];
    DataType *src4 = src[4];
    DataType *src5 = src[5];
    DataType *src6 = src[6];
    DataType *src7 = src[7];
    DataType *src8 = src[8];
    DataType *src9 = src[9];
    DataType *src10 = src[10];
    DataType *src11 = src[11];
    DataType *src12 = src[12];
    DataType *src13 = src[13];
    DataType *src14 = src[14];
    DataType *src15 = src[15];
    DataType *src16 = src[16];
    DataType *src17 = src[17];
    DataType *src18 = src[18];
    DataType *src19 = src[19];

    switch (num_blocks)
    {
    case 2:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i];
        }
        break;
    }
    case 3:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i];
        }
        break;
    }
    case 4:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i];
        }
        break;
    }
    case 5:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i];
        }
        break;
    }
    case 6:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i];
        }
        break;
    }
    case 7:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i];
        }
        break;
    }
    case 8:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i];
        }
        break;
    }
    case 9:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i];
        }
        break;
    }
    case 10:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i];
        }
        break;
    }
    case 11:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i];
        }
        break;
    }
    case 12:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i];
        }
        break;
    }
    case 13:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i];
        }
        break;
    }
    case 14:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i];
        }
        break;
    }
    case 15:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i] & src14[i];
        }
        break;
    }
    case 16:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i] & src14[i] & src15[i];
        }
        break;
    }
    case 17:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i] & src14[i] & src15[i] & src16[i];
        }
        break;
    }
    case 18:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i] & src14[i] & src15[i] & src16[i] & src17[i];
        }
        break;
    }
    case 19:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i] & src14[i] & src15[i] & src16[i] & src17[i] & src18[i];
        }
        break;
    }
    case 20:
    {
#pragma omp parallel for simd num_threads(PARALLEL_THREAD)
        for (size_t i = 0; i < num_elements; ++i)
        {
            dst[i] = src0[i] & src1[i] & src2[i] & src3[i] & src4[i] & src5[i] & src6[i] & src7[i] & src8[i] & src9[i] & src10[i] & src11[i] & src12[i] & src13[i] & src14[i] & src15[i] & src16[i] & src17[i] & src18[i] & src19[i];
        }
        break;
    }
    default:
        std::cerr << "Unknown num_blocks: " << num_blocks << std::endl;
        break;
    }
}

// 单纯的发送, 只负责安排工作, 不等待工作完成.
static size_t handle_send(MPI_Comm comm, MPI_Datatype datatype, std::vector<Operation> *ops, void *data, const FlexTree_Context &ft_ctx, MPI_Request request[])
{

    size_t start;
    size_t request_index = 0;

    for (const auto &i : *ops)
    {
        if (LIKELY(ft_ctx.node_label != i.peer))
        {
            for (const auto &j : i.blocks)
            {
                start = ft_ctx.split_size * j;
                //LOG_IF(INFO, node_label == 4) << "##4 send " << j << " which is " << start << "+" << count << " to " << i.peer ;
                //std::cout << node_label << " send " << j << " which is " << start << "+" << count << " to " << i.peer << ", element size = " << type_size << std::endl;
                if (UNLIKELY(j == ft_ctx.num_split - 1 && ft_ctx.data_size != ft_ctx.data_size_aligned))
                {
                    MPI_Isend(data + start * ft_ctx.type_size, ft_ctx.last_split_size, datatype, i.peer, 0, comm, &request[request_index++]); // 此处的tag暂时先打0
                }
                else 
                {
                    MPI_Isend(data + start * ft_ctx.type_size, ft_ctx.split_size, datatype, i.peer, 0, comm, &request[request_index++]); // 此处的tag暂时先打0
                }
            }
        }
    }
    return request_index;
}

// 同上, 只负责安排工作, 不等待工作完成.
// accordingly 参数的含义是, 如果为 true, 那么把数据块写到 buffer 中对应的位置去; 如果为 false, 那么直接平铺在 buffer 中.
static size_t handle_recv(MPI_Comm comm, MPI_Datatype datatype, std::vector<Operation> *ops, void *buffer, const FlexTree_Context &ft_ctx, bool accordingly, MPI_Request request[])
{

    size_t start = 0;
    size_t request_index = 0;

    for (const auto &i : *ops)
    {
        if (LIKELY(ft_ctx.node_label != i.peer))
        {
            for (const auto &j : i.blocks)
            {
                if (accordingly) 
                {
                    start = ft_ctx.split_size * j;
                }
                if (UNLIKELY(j == ft_ctx.num_split - 1 && ft_ctx.data_size != ft_ctx.data_size_aligned))
                {
                    MPI_Irecv(buffer + start * ft_ctx.type_size, ft_ctx.last_split_size, datatype, i.peer, 0, comm, &request[request_index++]); // 此处的tag暂时先打0
                }
                else
                {
                    MPI_Irecv(buffer + start * ft_ctx.type_size, ft_ctx.split_size, datatype, i.peer, 0, comm, &request[request_index++]); // 此处的tag暂时先打0
                }
                
                if (!accordingly)
                {
                    start += ft_ctx.split_size;
                }
            }
        }
    }
    return request_index;
}

// 负责进行加和, 然后放到指定的位置上去. 注意会自动包含自己的那块data.
static void handle_reduce(MPI_Datatype datatype, MPI_Op op, std::vector<size_t> *blocks, void *buffer, void *data, void *dst, const FlexTree_Context &ft_ctx, size_t num_peers, void *extra_buffer = nullptr, size_t extra_peers = 0)
{
    const size_t peer_gap = blocks->size() * ft_ctx.split_size;
    void **src = (void**)(new char*[ft_ctx.num_peers + 2]);
    for (int i = 0; i < num_peers + 2; i++)
    {
        src[i] = nullptr;
    }
    for (auto i = blocks->begin(); i != blocks->end(); i++)
    {
        size_t start = ft_ctx.split_size * (*i);
        size_t src_index = 1;
        src[0] = data + start * ft_ctx.type_size;
        if (dst == nullptr)
        {
            dst = src[0];
        }
        else
        {
            dst = dst + start * ft_ctx.type_size;
        }
        start = (i - blocks->begin()) * ft_ctx.split_size;
        for (size_t j = 0; j < num_peers; j++)
        {
            src[src_index++] = buffer + start * ft_ctx.type_size;
            start += peer_gap;
        }
        start = (i - blocks->begin()) * ft_ctx.split_size;
        for (size_t j = 0; j < extra_peers; j++)
        {
            src[src_index++] = extra_buffer + start * ft_ctx.type_size;
            start += peer_gap;
        }
        size_t split_size = ((*i) == ft_ctx.num_split - 1) ? ft_ctx.last_split_size : ft_ctx.split_size;
        if (op == MPI_SUM)
        {
            if (datatype == MPI_UINT8_T) reduce_sum((uint8_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT8_T) reduce_sum((int8_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_UINT16_T) reduce_sum((uint16_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT16_T) reduce_sum((int16_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT32_T) reduce_sum((int32_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT64_T) reduce_sum((int64_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_FLOAT) reduce_sum((float**)src, dst, src_index, split_size);
            else if (datatype == MPI_DOUBLE) reduce_sum((double**)src, dst, src_index, split_size);
            else if (datatype == MPI_C_BOOL) reduce_sum((bool**)src, dst, src_index, split_size);
            else if (datatype == MPI_LONG_LONG_INT) reduce_sum((long long int**)src, dst, src_index, split_size);
            else if (datatype == MPI_LONG_LONG) reduce_sum((long long**)src, dst, src_index, split_size);
            else 
            {
                char name[20];
                int name_len;
                MPI_Type_get_name(datatype, name, &name_len);
                name[name_len] = '\0';
                std::string s = name;
                std::cerr << "Type " << s << " is not supported in MPI mode." << std::endl;
                exit(0);
            }
        }
        else if (op == MPI_BAND)
        {
            if (datatype == MPI_UINT8_T) reduce_band((uint8_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT8_T) reduce_band((int8_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_UINT16_T) reduce_band((uint16_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT16_T) reduce_band((int16_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT32_T) reduce_band((int32_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_INT64_T) reduce_band((int64_t**)src, dst, src_index, split_size);
            else if (datatype == MPI_LONG_LONG_INT) reduce_band((long long int**)src, dst, src_index, split_size);
            else if (datatype == MPI_LONG_LONG) reduce_band((long long**)src, dst, src_index, split_size);
            else 
            {
                char name[20];
                int name_len;
                MPI_Type_get_name(datatype, name, &name_len);
                name[name_len] = '\0';
                std::string s = name;
                std::cerr << "Type " << s << " is not supported in MPI mode." << std::endl;
                exit(0);
            }
        }
        else 
        {
            std::cerr << "Unsupported op " << op << std::endl;
            exit(0);
        }
    }
    delete[] src;
}

static bool comm_only = false;
static void *recv_buffer = nullptr; //必须初始化

static void tree_allreduce(MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, void *data, void *dst, const FlexTree_Context &ft_ctx, std::vector<size_t> stages)
{
    #ifdef FT_DEBUF
    std::cout << "FT DEBUG: inside treeallre: op " << op << "; len = " << len << "; total = " << num_nodes << "; datatype = " << datatype << std::endl;
    #endif
    if (dst == nullptr)
    {
        dst = data;
    }
    //LOG_IF(WARNING, node_label == 0) << "gathering start";
    Send_Ops send_ops(num_nodes, num_lonely, node_label, stages);
    Recv_Ops recv_ops(num_nodes, num_lonely, node_label, stages);
    send_ops.generate_ops();
    recv_ops.generate_ops();
    MPI_Comm sub_comm = comm;
    const size_t MAX_COMM_SIZE = 2 * (ft_ctx.num_split - 1) * (ft_ctx.num_split);
    size_t request_index = 0;
    MPI_Request *requests = new MPI_Request[MAX_COMM_SIZE];
    MPI_Status *status = new MPI_Status[MAX_COMM_SIZE];
    size_t lonely_request_index = 0;
    MPI_Request *lonely_requests;
    int tmp;
#ifdef SHOW_TIME
    TIME_RESET();
#endif
    if (ft_ctx.node_label < ft_ctx.num_split)
    {
        if (ft_ctx.has_lonely)
        {
            lonely_requests = new MPI_Request[ft_ctx.num_lonely << 1];
            MPI_Comm_split(comm, 0, ft_ctx.node_label, &sub_comm); // 这个 0 是 magic number, 用来标注本组的颜色.
            // 如果要用, 则必须修改. lonely_request_index = handle_recv(comm, datatype, &(recv_ops.lonely_ops), data + len * type_size, ft_ctx, false, lonely_requests);
        }
        for (size_t i = 0; i != stages.size(); i++)
        {
            // 这一步判断是为什么呢? 是因为, 函数不会试图修改data的内容, 已经reduce的数据将会放在dst中; 而除了第一步之外, 发送的都是reduce后的数据, 所以第一步需要单独提出来.
            if (i == 0)
            {
                request_index = handle_send(comm, datatype, &(send_ops.ops[i]), data, ft_ctx, requests + request_index); //这里顺便重置了 index
            }
            else
            {
                request_index = handle_send(comm, datatype, &(send_ops.ops[i]), dst, ft_ctx, requests + request_index); //这里顺便重置了 index
            }
            tmp = handle_recv(comm, datatype, &(recv_ops.ops[i]), recv_buffer, ft_ctx, false, requests + request_index);
#ifdef FT_DEBUF
            std::cout << "FT DEBUG: start to send/recv" << std::endl;
#endif
            MPI_Waitall(tmp, requests + request_index, status);
#ifdef FT_DEBUF
            std::cout << "FT DEBUG: complete send/recv" << std::endl;
#endif
            if (lonely_request_index == 0 || i != stages.size() - 1)
            {
                handle_reduce(datatype, op, &(recv_ops.ops[i][0].blocks), recv_buffer, data, dst, ft_ctx, recv_ops.ops[i].size() - 1);
            }
            else
            {
                MPI_Waitall(lonely_request_index, lonely_requests, status);
#ifdef SHOW_TIME
                TIME_LOG_IF(node_label == 0, "node 0 lonely gather finished");
#endif SHOW_TIME
                // 如果要用, 则必须修改. handle_reduce(datatype, op, &(recv_ops.ops[i][0].blocks), recv_buffer, data, dst, ft_ctx, recv_ops.ops[i].size() - 1, data + ft_ctx.len * ft_ctx.type_size, ft_ctx.num_lonely);
            }
            MPI_Waitall(request_index, requests, status);
            MPI_Barrier(sub_comm);
        }
#ifdef SHOW_TIME
            TIME_LOG_IF(node_label == 0, "(left) FT gather finished");
#endif SHOW_TIME
        if (ft_ctx.has_lonely) MPI_Barrier(comm);
        //LOG_IF(WARNING, node_label == 0) << "gathering done";
#ifdef SHOW_TIME
        TIME_RESET();
#endif
#ifdef FT_DEBUF
        std::cout << "FT DEBUG: complete reduce" << std::endl;
#endif
        if (ft_ctx.has_lonely)
        {
            // 测试是否和内存锁有关系
            //size_t start = len / num_split * node_label;
            //memcpy(recv_buffer + start, data + start, sizeof(DataType) * len / num_split);
            //lonely_request_index = handle_send(&(recv_ops.lonely_ops), recv_buffer, len, num_split, node_label, lonely_requests);
            // end
            //lonely_request_index = handle_send(&(recv_ops.lonely_ops), data, len, num_split, node_label, lonely_requests);
        }
        for (int i = stages.size() - 1; i >= 0; i--)
        {
            if (i == 0 && ft_ctx.has_lonely)
            {
                // 如果要用, 则必须修改. lonely_request_index = handle_send(comm, datatype, &(recv_ops.lonely_ops), data, ft_ctx, lonely_requests);
            }
            request_index = handle_send(comm, datatype, &(recv_ops.ops[i]), dst, ft_ctx, requests);
            request_index += handle_recv(comm, datatype, &(send_ops.ops[i]), dst, ft_ctx, true, requests + request_index);
            MPI_Waitall(request_index, requests, status);
            MPI_Barrier(sub_comm);
        }
#ifdef SHOW_TIME
                TIME_LOG_IF(node_label == 0, "(left comm) FT broadcast finished");
#endif SHOW_TIME
        if (ft_ctx.has_lonely)
        {
            MPI_Waitall(lonely_request_index, lonely_requests, status);
#ifdef SHOW_TIME
            TIME_LOG_IF(node_label == 0, "node 0 lonely broadcast finished");
#endif SHOW_TIME
            MPI_Barrier(comm);
            delete[] lonely_requests;
        }
    }
    else 
    {
        lonely_requests = new MPI_Request[ft_ctx.num_split << 2];
        MPI_Comm_split(comm, 1, ft_ctx.node_label, &sub_comm); // 这个 1 是 magic number, 用来标注本组的颜色.
        //LOG(WARNING) << "LONELY send start";
        // 如果要用, 则必须修改. lonely_request_index = handle_send(comm, datatype, &(send_ops.lonely_ops), data, ft_ctx, lonely_requests);
        MPI_Waitall(lonely_request_index, lonely_requests, status);
#ifdef SHOW_TIME
        TIME_LOG_IF(true, "(right) lonely send finished");
#endif SHOW_TIME
        //LOG(WARNING) << "LONELY send done";
        MPI_Barrier(comm);
#ifdef SHOW_TIME
        TIME_RESET();
#endif
        //LOG(WARNING) << "LONELY recv start";
        // 如果要用, 则必须修改. lonely_request_index = handle_recv(comm, datatype, &(send_ops.lonely_ops), data, ft_ctx, true, lonely_requests);
        MPI_Waitall(lonely_request_index, lonely_requests, status);
#ifdef SHOW_TIME
        TIME_LOG_IF(true, "lonely recv finished");
#endif SHOW_TIME
        MPI_Barrier(comm);
        delete[] lonely_requests;
    }
    delete[] requests;
    delete[] status;
#ifdef FT_DEBUF
    std::cout << "FT DEBUG: complete allreduce" << std::endl;
#endif
    //LOG_IF(WARNING, node_label == 0) << "broadcast done";
}

static void* flextree_register_the_buffer(size_t _size)
{
    static void* buffer;
    static size_t size = 0;
    if (_size > size)
    {
        if (size != 0)
        {
            delete[] buffer;
        }
        buffer = (void*)(new char[_size]);
        size = _size;
    }
    return buffer;
}

#ifndef OMPI_MPI_H
int MPI_Allreduce_FT(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
#else
static int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
#endif
{
#ifdef FT_DEBUG
    std::cout << "FlexTree AR called" << std::endl;
#endif
    const FlexTree_Context ft_ctx(comm, datatype, count);

    if (ft_ctx.num_nodes <= 1)
    {
        if (sendbuf != MPI_IN_PLACE)
        {
            memcpy(recvbuf, sendbuf, count * type_size);
        }
        return 0;
    }

    recv_buffer = flextree_register_the_buffer(ft_ctx.data_size_aligned);
    
    void *recv_buffer_aligned = (void*)(new char[(count_aligned * type_size) << 1]);
    // MPI_IN_PLACE
    if (sendbuf != MPI_IN_PLACE)
    {
        memcpy(recvbuf, sendbuf, count * type_size);
    }

    memcpy(recv_buffer_aligned, recvbuf, count * type_size);

    tree_allreduce(datatype, op, comm, recv_buffer_aligned, ft_ctx, {num_nodes});

    memcpy(recvbuf, recv_buffer_aligned, count * type_size);

    delete[] recv_buffer;
    delete[] recv_buffer_aligned;
    return 0;
}

#ifndef OMPI_MPI_H

// util
template<typename T>
void write_vector_to_file(std::vector<T> vec, std::string filename)
{
    std::ofstream f(filename, std::ios::out);
    for (auto &i : vec)
    {
        f << i << std::endl;
    }
    f.close();
}

int main(int argc, char **argv)
{
        // 当前节点的编号, 总结点数量, 孤立节点数量
    size_t node_label, total_peers, num_lonely = 0; 

    // 命令行参数
    int repeat = 1;
    double sum_time = 0, min_time = INF;
    int comm_type = 0; // 0 for tree, 1 for ring, 2 for mpi
    bool to_file = false;

    // others
    std::vector<double> repeat_time;
    int tmp;

    // init mpi
    MPI_Init_thread(&argc,&argv, MPI_THREAD_MULTIPLE, &tmp);
    MPI_Comm_size(MPI_COMM_WORLD, &tmp);
    total_peers = tmp;
    MPI_Comm_rank(MPI_COMM_WORLD, &tmp);
    node_label = tmp;
    // end init

    MPI_Barrier(MPI_COMM_WORLD);
    size_t data_len = 336e4;
    std::vector<size_t> topo;
        
    // 初始化 data 和 buffer
    int32_t *data = new int32_t[data_len * 2];
    for (size_t i = 0; i != data_len; i++)
    {
        data[i] = i / 1.0;
    }
    auto recv_buffer =(void*)(new char[data_len<<4]);
    // 准备就绪
    {
        for (auto i = 0; i < repeat; i++)
        {
            MPI_Barrier(MPI_COMM_WORLD);
            auto time1 = MPI_Wtime();
            MPI_Allreduce_FT(data, recv_buffer, data_len, MPI_INT32_T, MPI_SUM, MPI_COMM_WORLD);
            auto time2 = MPI_Wtime();
            repeat_time.push_back(time2 - time1);
            sum_time += time2 - time1;
            min_time = std::min(time2 - time1, min_time);
            memcpy(data, recv_buffer, data_len * sizeof(float));
        }
    }

    for (int i = 0; i != total_peers; i++)
    {
        MPI_Barrier(MPI_COMM_WORLD);
        if (i == node_label)
        {
            std::cout << "CHECK " << node_label << ": ";
            for (int i = 9; i != 20; i++) std::cout << data[i] << " ";
            std::cout << std::endl;
        }
    }
    
    MPI_Finalize();

    // 写入文件
    if (node_label == 0 && to_file)
    {
        std::stringstream ss;
        ss << total_peers << "." << data_len << ".";
        for (auto i : topo)
        {
            ss << i << "-";
        }
        ss << (comm_only ? ".comm_test." : ".ar_test.");
        ss << time(NULL) << ".txt";
        std::string filename;
        ss >> filename;
        write_vector_to_file(repeat_time, filename);
    }

    return 0;
}
#endif //end if of check whether in mpi.h
#endif //end if of check c++

#endif
//end of flextree mod