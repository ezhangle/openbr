#include "opencv2/core/core.hpp"
#include "opencv2/core/internal.hpp"

using cv::Size;
using cv::Mat;
using cv::Point;
using cv::FileStorage;
using cv::Rect;
using cv::Ptr;
using cv::FileNode;
using cv::Mat_;
using cv::Range;
using cv::FileNodeIterator;
using cv::ParallelLoopBody;

#include "boost.h"
#include <queue>
#include "cxmisc.h"

#define CC_BOOST            "boost"
#define CC_WEAK_COUNT       "weakCount"
#define CC_STAGE_THRESHOLD  "stageThreshold"
#define CC_WEAK_CLASSIFIERS "weakClassifiers"
#define CC_INTERNAL_NODES   "internalNodes"
#define CC_LEAF_VALUES      "leafValues"

using namespace std;

static inline double
logRatio( double val )
{
    const double eps = 1e-5;

    val = max( val, eps );
    val = min( val, 1. - eps );
    return log( val/(1. - val) );
}

#define CV_CMP_NUM_IDX(i,j) (aux[i] < aux[j])
static CV_IMPLEMENT_QSORT_EX( icvSortIntAux, int, CV_CMP_NUM_IDX, const float* )
static CV_IMPLEMENT_QSORT_EX( icvSortUShAux, unsigned short, CV_CMP_NUM_IDX, const float* )

#define CV_THRESHOLD_EPS (0.00001F)

static const int MinBlockSize = 1 << 16;
static const int BlockSizeDelta = 1 << 10;

// TODO remove this code duplication with ml/precomp.hpp

static int CV_CDECL icvCmpIntegers( const void* a, const void* b )
{
    return *(const int*)a - *(const int*)b;
}

static CvMat* cvPreprocessIndexArray( const CvMat* idx_arr, int data_arr_size, bool check_for_duplicates=false )
{
    CvMat* idx = 0;

    CV_FUNCNAME( "cvPreprocessIndexArray" );

    __BEGIN__;

    int i, idx_total, idx_selected = 0, step, type, prev = INT_MIN, is_sorted = 1;
    uchar* srcb = 0;
    int* srci = 0;
    int* dsti;

    if( !CV_IS_MAT(idx_arr) )
        CV_ERROR( CV_StsBadArg, "Invalid index array" );

    if( idx_arr->rows != 1 && idx_arr->cols != 1 )
        CV_ERROR( CV_StsBadSize, "the index array must be 1-dimensional" );

    idx_total = idx_arr->rows + idx_arr->cols - 1;
    srcb = idx_arr->data.ptr;
    srci = idx_arr->data.i;

    type = CV_MAT_TYPE(idx_arr->type);
    step = CV_IS_MAT_CONT(idx_arr->type) ? 1 : idx_arr->step/CV_ELEM_SIZE(type);

    switch( type )
    {
    case CV_8UC1:
    case CV_8SC1:
        // idx_arr is array of 1's and 0's -
        // i.e. it is a mask of the selected components
        if( idx_total != data_arr_size )
            CV_ERROR( CV_StsUnmatchedSizes,
            "Component mask should contain as many elements as the total number of input variables" );

        for( i = 0; i < idx_total; i++ )
            idx_selected += srcb[i*step] != 0;

        if( idx_selected == 0 )
            CV_ERROR( CV_StsOutOfRange, "No components/input_variables is selected!" );

        break;
    case CV_32SC1:
        // idx_arr is array of integer indices of selected components
        if( idx_total > data_arr_size )
            CV_ERROR( CV_StsOutOfRange,
            "index array may not contain more elements than the total number of input variables" );
        idx_selected = idx_total;
        // check if sorted already
        for( i = 0; i < idx_total; i++ )
        {
            int val = srci[i*step];
            if( val >= prev )
            {
                is_sorted = 0;
                break;
            }
            prev = val;
        }
        break;
    default:
        CV_ERROR( CV_StsUnsupportedFormat, "Unsupported index array data type "
                                           "(it should be 8uC1, 8sC1 or 32sC1)" );
    }

    CV_CALL( idx = cvCreateMat( 1, idx_selected, CV_32SC1 ));
    dsti = idx->data.i;

    if( type < CV_32SC1 )
    {
        for( i = 0; i < idx_total; i++ )
            if( srcb[i*step] )
                *dsti++ = i;
    }
    else
    {
        for( i = 0; i < idx_total; i++ )
            dsti[i] = srci[i*step];

        if( !is_sorted )
            qsort( dsti, idx_total, sizeof(dsti[0]), icvCmpIntegers );

        if( dsti[0] < 0 || dsti[idx_total-1] >= data_arr_size )
            CV_ERROR( CV_StsOutOfRange, "the index array elements are out of range" );

        if( check_for_duplicates )
        {
            for( i = 1; i < idx_total; i++ )
                if( dsti[i] <= dsti[i-1] )
                    CV_ERROR( CV_StsBadArg, "There are duplicated index array elements" );
        }
    }

    __END__;

    if( cvGetErrStatus() < 0 )
        cvReleaseMat( &idx );

    return idx;
}

namespace br
{

//----------------------------- CascadeBoostParams -------------------------------------------------

CascadeBoostParams::CascadeBoostParams() : minTAR( 0.995F), maxFAR( 0.5F )
{
    boost_type = CvBoost::GENTLE;
    maxCatCount = 0;
    use_surrogates = use_1se_rule = truncate_pruned_tree = false;
}

CascadeBoostParams::CascadeBoostParams( int _boostType, int _maxCatCount, float _minTAR, float _maxFAR,
                                        double _weightTrimRate, int _maxDepth, int _maxWeakCount ) :
    CvBoostParams( _boostType, _maxWeakCount, _weightTrimRate, _maxDepth, false, 0 )
{
    boost_type = CvBoost::GENTLE;
    maxCatCount = _maxCatCount;
    minTAR = _minTAR;
    maxFAR = _maxFAR;
    use_surrogates = use_1se_rule = truncate_pruned_tree = false;
}

void CascadeBoostParams::store(QDataStream &stream) const
{
    stream << boost_type;
    stream << minTAR;
    stream << maxFAR;
    stream << weight_trim_rate;
    stream << max_depth;
    stream << weak_count;
}

void CascadeBoostParams::load(QDataStream &stream)
{
    stream >> boost_type;
    stream >> minTAR;
    stream >> maxFAR;
    stream >> weight_trim_rate;
    stream >> max_depth;
    stream >> weak_count;
}

//---------------------------- CascadeDataStorage -----------------------------

CascadeDataStorage::CascadeDataStorage(Representation *_rep, int numSamples)
{
    data.create(numSamples, _rep->getSize().area(), CV_32FC1);
    labels.create(1, numSamples, CV_32FC1);
    rep = _rep;
}

void CascadeDataStorage::setImage(const cv::Mat &sample, float label, int idx)
{
    if (idx > data.rows)
        qFatal("invalid index %d to cascade data of size %d", idx, data.rows);
    Mat pre = rep->preprocess(sample);
    Mat flat(1, pre.rows * pre.cols, CV_32FC1, pre.data);

    flat.copyTo(data.row(idx));
    labels.at<float>(idx) = label;
}

void CascadeDataStorage::freeTrainData()
{
    data.create(1, rep->getSize().area(), CV_32FC1);
    labels.create(1, 1, CV_32FC1);
}

float CascadeDataStorage::response(int featureIdx, int sampleIdx) const
{
    Mat sample(rep->getSize(), CV_32FC1, data.row(sampleIdx).data);
    return rep->evaluate(sample, QList<int>() << featureIdx).at<float>(0);
}

//---------------------------- CascadeBoostTrainData -----------------------------

CascadeBoostTrainData::CascadeBoostTrainData( const CascadeDataStorage* _storage, const CvDTreeParams& _params )
{
    is_classifier = true;
    var_all = var_count = (int)_storage->numFeatures();

    storage = _storage;
    shared = true;
    set_params( _params );

    const int maxCatCount = ((CascadeBoostParams *)&_params)->maxCatCount;

    max_c_count = MAX( 2, maxCatCount );
    var_type = cvCreateMat( 1, var_count + 2, CV_32SC1 );
    if ( maxCatCount > 0 )
    {
        numPrecalcIdx = 0;
        cat_var_count = var_count;
        ord_var_count = 0;
        for( int vi = 0; vi < var_count; vi++ )
        {
            var_type->data.i[vi] = vi;
        }
    }
    else
    {
        cat_var_count = 0;
        ord_var_count = var_count;
        for( int vi = 1; vi <= var_count; vi++ )
        {
            var_type->data.i[vi-1] = -vi;
        }
    }
    var_type->data.i[var_count] = cat_var_count;
    var_type->data.i[var_count+1] = cat_var_count+1;

    int maxSplitSize = cvAlign(sizeof(CvDTreeSplit) + (MAX(0,max_c_count - 33)/32)*sizeof(int),sizeof(void*));
    int treeBlockSize = MAX((int)sizeof(CvDTreeNode)*8, maxSplitSize);
    treeBlockSize = MAX(treeBlockSize + BlockSizeDelta, MinBlockSize);
    tree_storage = cvCreateMemStorage( treeBlockSize );
    node_heap = cvCreateSet( 0, sizeof(node_heap[0]), sizeof(CvDTreeNode), tree_storage );
    split_heap = cvCreateSet( 0, sizeof(split_heap[0]), maxSplitSize, tree_storage );
}

CascadeBoostTrainData::CascadeBoostTrainData( const CascadeDataStorage* _storage,
                                                 int _numSamples,
                                                 int _precalcValBufSize, int _precalcIdxBufSize,
                                                 const CvDTreeParams& _params )
{
    int* idst = 0;
    unsigned short* udst = 0;

    uint64 effective_buf_size = 0;
    int effective_buf_height = 0, effective_buf_width = 0;


    clear();
    shared = true;
    have_labels = true;
    have_priors = false;
    is_classifier = true;

    rng = &cv::theRNG();

    set_params( _params );

    CV_Assert( _storage );
    storage = _storage;
    const int maxCatCount = ((CascadeBoostParams *)&_params)->maxCatCount;

    max_c_count = MAX( 2, maxCatCount );
    _resp = storage->labels;
    responses = &_resp;
    // TODO: check responses: elements must be 0 or 1

    if( _precalcValBufSize < 0 || _precalcIdxBufSize < 0)
        CV_Error( CV_StsOutOfRange, "_numPrecalcVal and _numPrecalcIdx must be positive or 0" );

    var_count = var_all = storage->numFeatures();
    sample_count = _numSamples;

    is_buf_16u = false;
    if (sample_count < 65536)
        is_buf_16u = true;

    numPrecalcVal = min( cvRound((double)_precalcValBufSize*1048576. / (sizeof(float)*sample_count)), var_count );
    numPrecalcIdx = min( cvRound((double)_precalcIdxBufSize*1048576. /
                ((is_buf_16u ? sizeof(unsigned short) : sizeof (int))*sample_count)), var_count );

    assert( numPrecalcIdx >= 0 && numPrecalcVal >= 0 );

    valCache.create( numPrecalcVal, sample_count, CV_32FC1 );
    var_type = cvCreateMat( 1, var_count + 2, CV_32SC1 );
    if ( maxCatCount > 0 )
    {
        numPrecalcIdx = 0;
        cat_var_count = var_count;
        ord_var_count = 0;
        for( int vi = 0; vi < var_count; vi++ )
        {
            var_type->data.i[vi] = vi;
        }
    }
    else
    {
        cat_var_count = 0;
        ord_var_count = var_count;
        for( int vi = 1; vi <= var_count; vi++ )
        {
            var_type->data.i[vi-1] = -vi;
        }
    }
    var_type->data.i[var_count] = cat_var_count;
    var_type->data.i[var_count+1] = cat_var_count+1;
    work_var_count = ( cat_var_count ? 0 : numPrecalcIdx ) + 1/*cv_lables*/;
    buf_count = 2;

    buf_size = -1; // the member buf_size is obsolete

    effective_buf_size = (uint64)(work_var_count + 1)*(uint64)sample_count * buf_count; // this is the total size of "CvMat buf" to be allocated
    effective_buf_width = sample_count;
    effective_buf_height = work_var_count+1;

    if (effective_buf_width >= effective_buf_height)
        effective_buf_height *= buf_count;
    else
        effective_buf_width *= buf_count;

    if ((uint64)effective_buf_width * (uint64)effective_buf_height != effective_buf_size)
    {
        CV_Error(CV_StsBadArg, "The memory buffer cannot be allocated since its size exceeds integer fields limit");
    }
    if ( is_buf_16u )
        buf = cvCreateMat( effective_buf_height, effective_buf_width, CV_16UC1 );
    else
        buf = cvCreateMat( effective_buf_height, effective_buf_width, CV_32SC1 );

    cat_count = cvCreateMat( 1, cat_var_count + 1, CV_32SC1 );

    // precalculate valCache and set indices in buf
    precalculate();

    // now calculate the maximum size of split,
    // create memory storage that will keep nodes and splits of the decision tree
    // allocate root node and the buffer for the whole training data
    int maxSplitSize = cvAlign(sizeof(CvDTreeSplit) +
        (MAX(0,sample_count - 33)/32)*sizeof(int),sizeof(void*));
    int treeBlockSize = MAX((int)sizeof(CvDTreeNode)*8, maxSplitSize);
    treeBlockSize = MAX(treeBlockSize + BlockSizeDelta, MinBlockSize);
    tree_storage = cvCreateMemStorage( treeBlockSize );
    node_heap = cvCreateSet( 0, sizeof(*node_heap), sizeof(CvDTreeNode), tree_storage );

    int nvSize = var_count*sizeof(int);
    nvSize = cvAlign(MAX( nvSize, (int)sizeof(CvSetElem) ), sizeof(void*));
    int tempBlockSize = nvSize;
    tempBlockSize = MAX( tempBlockSize + BlockSizeDelta, MinBlockSize );
    temp_storage = cvCreateMemStorage( tempBlockSize );
    nv_heap = cvCreateSet( 0, sizeof(*nv_heap), nvSize, temp_storage );

    data_root = new_node( 0, sample_count, 0, 0 );

    // set sample labels
    if (is_buf_16u)
        udst = (unsigned short*)(buf->data.s + work_var_count*sample_count);
    else
        idst = buf->data.i + work_var_count*sample_count;

    for (int si = 0; si < sample_count; si++)
    {
        if (udst)
            udst[si] = (unsigned short)si;
        else
            idst[si] = si;
    }
    for( int vi = 0; vi < var_count; vi++ )
        data_root->set_num_valid(vi, sample_count);
    for( int vi = 0; vi < cat_var_count; vi++ )
        cat_count->data.i[vi] = max_c_count;

    cat_count->data.i[cat_var_count] = 2;

    maxSplitSize = cvAlign(sizeof(CvDTreeSplit) +
        (MAX(0,max_c_count - 33)/32)*sizeof(int),sizeof(void*));
    split_heap = cvCreateSet( 0, sizeof(*split_heap), maxSplitSize, tree_storage );

    priors = cvCreateMat( 1, get_num_classes(), CV_64F );
    cvSet(priors, cvScalar(1));
    priors_mult = cvCloneMat( priors );
    counts = cvCreateMat( 1, get_num_classes(), CV_32SC1 );
    direction = cvCreateMat( 1, sample_count, CV_8UC1 );
    split_buf = cvCreateMat( 1, sample_count, CV_32SC1 );//TODO: make a pointer
}

void CascadeBoostTrainData::free_train_data()
{
    CvDTreeTrainData::free_train_data();
    valCache.release();
}

CvDTreeNode* CascadeBoostTrainData::subsample_data( const CvMat* _subsample_idx )
{
    CvDTreeNode* root = 0;
    CvMat* isubsample_idx = 0;
    CvMat* subsample_co = 0;

    bool isMakeRootCopy = true;

    if( !data_root )
        CV_Error( CV_StsError, "No training data has been set" );

    if( _subsample_idx )
    {
        CV_Assert( (isubsample_idx = cvPreprocessIndexArray( _subsample_idx, sample_count )) != 0 );

        if( isubsample_idx->cols + isubsample_idx->rows - 1 == sample_count )
        {
            const int* sidx = isubsample_idx->data.i;
            for( int i = 0; i < sample_count; i++ )
            {
                if( sidx[i] != i )
                {
                    isMakeRootCopy = false;
                    break;
                }
            }
        }
        else
            isMakeRootCopy = false;
    }

    if( isMakeRootCopy )
    {
        // make a copy of the root node
        CvDTreeNode temp;
        int i;
        root = new_node( 0, 1, 0, 0 );
        temp = *root;
        *root = *data_root;
        root->num_valid = temp.num_valid;
        if( root->num_valid )
        {
            for( i = 0; i < var_count; i++ )
                root->num_valid[i] = data_root->num_valid[i];
        }
        root->cv_Tn = temp.cv_Tn;
        root->cv_node_risk = temp.cv_node_risk;
        root->cv_node_error = temp.cv_node_error;
    }
    else
    {
        int* sidx = isubsample_idx->data.i;
        // co - array of count/offset pairs (to handle duplicated values in _subsample_idx)
        int* co, cur_ofs = 0;
        int workVarCount = get_work_var_count();
        int count = isubsample_idx->rows + isubsample_idx->cols - 1;

        root = new_node( 0, count, 1, 0 );

        CV_Assert( (subsample_co = cvCreateMat( 1, sample_count*2, CV_32SC1 )) != 0);
        cvZero( subsample_co );
        co = subsample_co->data.i;
        for( int i = 0; i < count; i++ )
            co[sidx[i]*2]++;
        for( int i = 0; i < sample_count; i++ )
        {
            if( co[i*2] )
            {
                co[i*2+1] = cur_ofs;
                cur_ofs += co[i*2];
            }
            else
                co[i*2+1] = -1;
        }

        cv::AutoBuffer<uchar> inn_buf(sample_count*(2*sizeof(int) + sizeof(float)));
        // subsample ordered variables
        for( int vi = 0; vi < numPrecalcIdx; vi++ )
        {
            int ci = get_var_type(vi);
            CV_Assert( ci < 0 );

            int *src_idx_buf = (int*)(uchar*)inn_buf;
            float *src_val_buf = (float*)(src_idx_buf + sample_count);
            int* sample_indices_buf = (int*)(src_val_buf + sample_count);
            const int* src_idx = 0;
            const float* src_val = 0;
            get_ord_var_data( data_root, vi, src_val_buf, src_idx_buf, &src_val, &src_idx, sample_indices_buf );

            int j = 0, idx, count_i;
            int num_valid = data_root->get_num_valid(vi);
            CV_Assert( num_valid == sample_count );

            if (is_buf_16u)
            {
                unsigned short* udst_idx = (unsigned short*)(buf->data.s + root->buf_idx*get_length_subbuf() +
                    vi*sample_count + data_root->offset);
                for( int i = 0; i < num_valid; i++ )
                {
                    idx = src_idx[i];
                    count_i = co[idx*2];
                    if( count_i )
                        for( cur_ofs = co[idx*2+1]; count_i > 0; count_i--, j++, cur_ofs++ )
                            udst_idx[j] = (unsigned short)cur_ofs;
                }
            }
            else
            {
                int* idst_idx = buf->data.i + root->buf_idx*get_length_subbuf() +
                    vi*sample_count + root->offset;
                for( int i = 0; i < num_valid; i++ )
                {
                    idx = src_idx[i];
                    count_i = co[idx*2];
                    if( count_i )
                        for( cur_ofs = co[idx*2+1]; count_i > 0; count_i--, j++, cur_ofs++ )
                            idst_idx[j] = cur_ofs;
                }
            }
        }

        // subsample cv_lables
        const int* src_lbls = get_cv_labels(data_root, (int*)(uchar*)inn_buf);
        if (is_buf_16u)
        {
            unsigned short* udst = (unsigned short*)(buf->data.s + root->buf_idx*get_length_subbuf() +
                (workVarCount-1)*sample_count + root->offset);
            for( int i = 0; i < count; i++ )
                udst[i] = (unsigned short)src_lbls[sidx[i]];
        }
        else
        {
            int* idst = buf->data.i + root->buf_idx*get_length_subbuf() +
                (workVarCount-1)*sample_count + root->offset;
            for( int i = 0; i < count; i++ )
                idst[i] = src_lbls[sidx[i]];
        }

        // subsample sample_indices
        const int* sample_idx_src = get_sample_indices(data_root, (int*)(uchar*)inn_buf);
        if (is_buf_16u)
        {
            unsigned short* sample_idx_dst = (unsigned short*)(buf->data.s + root->buf_idx*get_length_subbuf() +
                workVarCount*sample_count + root->offset);
            for( int i = 0; i < count; i++ )
                sample_idx_dst[i] = (unsigned short)sample_idx_src[sidx[i]];
        }
        else
        {
            int* sample_idx_dst = buf->data.i + root->buf_idx*get_length_subbuf() +
                workVarCount*sample_count + root->offset;
            for( int i = 0; i < count; i++ )
                sample_idx_dst[i] = sample_idx_src[sidx[i]];
        }

        for( int vi = 0; vi < var_count; vi++ )
            root->set_num_valid(vi, count);
    }

    cvReleaseMat( &isubsample_idx );
    cvReleaseMat( &subsample_co );

    return root;
}

const int* CascadeBoostTrainData::get_class_labels( CvDTreeNode* n, int* labelsBuf)
{
    int nodeSampleCount = n->sample_count;
    int rStep = CV_IS_MAT_CONT( responses->type ) ? 1 : responses->step / CV_ELEM_SIZE( responses->type );

    int* sampleIndicesBuf = labelsBuf; //
    const int* sampleIndices = get_sample_indices(n, sampleIndicesBuf);
    for( int si = 0; si < nodeSampleCount; si++ )
    {
        int sidx = sampleIndices[si];
        labelsBuf[si] = (int)responses->data.fl[sidx*rStep];
    }
    return labelsBuf;
}

const int* CascadeBoostTrainData::get_sample_indices( CvDTreeNode* n, int* indicesBuf )
{
    return CvDTreeTrainData::get_cat_var_data( n, get_work_var_count(), indicesBuf );
}

const int* CascadeBoostTrainData::get_cv_labels( CvDTreeNode* n, int* labels_buf )
{
    return CvDTreeTrainData::get_cat_var_data( n, get_work_var_count() - 1, labels_buf );
}

void CascadeBoostTrainData::get_ord_var_data( CvDTreeNode* n, int vi, float* ordValuesBuf, int* sortedIndicesBuf,
        const float** ordValues, const int** sortedIndices, int* sampleIndicesBuf )
{
    int nodeSampleCount = n->sample_count;
    const int* sampleIndices = get_sample_indices(n, sampleIndicesBuf);

    if ( vi < numPrecalcIdx )
    {
        if( !is_buf_16u )
            *sortedIndices = buf->data.i + n->buf_idx*get_length_subbuf() + vi*sample_count + n->offset;
        else
        {
            const unsigned short* shortIndices = (const unsigned short*)(buf->data.s + n->buf_idx*get_length_subbuf() +
                                                    vi*sample_count + n->offset );
            for( int i = 0; i < nodeSampleCount; i++ )
                sortedIndicesBuf[i] = shortIndices[i];

            *sortedIndices = sortedIndicesBuf;
        }

        if( vi < numPrecalcVal )
        {
            for( int i = 0; i < nodeSampleCount; i++ )
            {
                int idx = (*sortedIndices)[i];
                idx = sampleIndices[idx];
                ordValuesBuf[i] =  valCache.at<float>( vi, idx );
            }
        }
        else
        {
            for( int i = 0; i < nodeSampleCount; i++ )
            {
                int idx = (*sortedIndices)[i];
                idx = sampleIndices[idx];
                ordValuesBuf[i] = storage->response( vi, idx );
            }
        }
    }
    else // vi >= numPrecalcIdx
    {
        cv::AutoBuffer<float> abuf(nodeSampleCount);
        float* sampleValues = &abuf[0];

        if ( vi < numPrecalcVal )
        {
            for( int i = 0; i < nodeSampleCount; i++ )
            {
                sortedIndicesBuf[i] = i;
                sampleValues[i] = valCache.at<float>( vi, sampleIndices[i] );
            }
        }
        else
        {
            for( int i = 0; i < nodeSampleCount; i++ )
            {
                sortedIndicesBuf[i] = i;
                sampleValues[i] = storage->response( vi, sampleIndices[i] );
            }
        }
        icvSortIntAux( sortedIndicesBuf, nodeSampleCount, &sampleValues[0] );
        for( int i = 0; i < nodeSampleCount; i++ )
            ordValuesBuf[i] = (&sampleValues[0])[sortedIndicesBuf[i]];
        *sortedIndices = sortedIndicesBuf;
    }

    *ordValues = ordValuesBuf;
}

const int* CascadeBoostTrainData::get_cat_var_data( CvDTreeNode* n, int vi, int* catValuesBuf )
{
    int nodeSampleCount = n->sample_count;
    int* sampleIndicesBuf = catValuesBuf; //
    const int* sampleIndices = get_sample_indices(n, sampleIndicesBuf);

    if ( vi < numPrecalcVal )
    {
        for( int i = 0; i < nodeSampleCount; i++ )
            catValuesBuf[i] = (int) valCache.at<float>( vi, sampleIndices[i]);
    }
    else
    {
        if( vi >= numPrecalcVal && vi < var_count )
        {
            for( int i = 0; i < nodeSampleCount; i++ )
                catValuesBuf[i] = (int)storage->response( vi, sampleIndices[i] );
        }
        else
        {
            get_cv_labels( n, catValuesBuf );
        }
    }

    return catValuesBuf;
}

float CascadeBoostTrainData::getVarValue( int vi, int si )
{
    if ( vi < numPrecalcVal && !valCache.empty() )
        return valCache.at<float>( vi, si );
    return storage->response( vi, si );
}


struct FeatureIdxOnlyPrecalc : ParallelLoopBody
{
    FeatureIdxOnlyPrecalc( const CascadeDataStorage* _storage, CvMat* _buf, int _sample_count, bool _is_buf_16u )
    {
        storage = _storage;
        sample_count = _sample_count;
        udst = (unsigned short*)_buf->data.s;
        idst = _buf->data.i;
        is_buf_16u = _is_buf_16u;
    }
    void operator()( const Range& range ) const
    {
        cv::AutoBuffer<float> valCache(sample_count);
        float* valCachePtr = (float*)valCache;
        for ( int fi = range.start; fi < range.end; fi++)
        {
            for( int si = 0; si < sample_count; si++ )
            {
                valCachePtr[si] = storage->response( fi, si );
                if ( is_buf_16u )
                    *(udst + fi*sample_count + si) = (unsigned short)si;
                else
                    *(idst + fi*sample_count + si) = si;
            }
            if ( is_buf_16u )
                icvSortUShAux( udst + fi*sample_count, sample_count, valCachePtr );
            else
                icvSortIntAux( idst + fi*sample_count, sample_count, valCachePtr );
        }
    }
    const CascadeDataStorage* storage;
    int sample_count;
    int* idst;
    unsigned short* udst;
    bool is_buf_16u;
};

struct FeatureValAndIdxPrecalc : ParallelLoopBody
{
    FeatureValAndIdxPrecalc( const CascadeDataStorage* _storage, CvMat* _buf, Mat* _valCache, int _sample_count, bool _is_buf_16u )
    {
        storage = _storage;
        valCache = _valCache;
        sample_count = _sample_count;
        udst = (unsigned short*)_buf->data.s;
        idst = _buf->data.i;
        is_buf_16u = _is_buf_16u;
    }
    void operator()( const Range& range ) const
    {
        for ( int fi = range.start; fi < range.end; fi++)
        {
            for( int si = 0; si < sample_count; si++ )
            {
                valCache->at<float>(fi,si) = storage->response( fi, si );
                if ( is_buf_16u )
                    *(udst + fi*sample_count + si) = (unsigned short)si;
                else
                    *(idst + fi*sample_count + si) = si;
            }
            if ( is_buf_16u )
                icvSortUShAux( udst + fi*sample_count, sample_count, valCache->ptr<float>(fi) );
            else
                icvSortIntAux( idst + fi*sample_count, sample_count, valCache->ptr<float>(fi) );
        }
    }
    const CascadeDataStorage* storage;
    Mat* valCache;
    int sample_count;
    int* idst;
    unsigned short* udst;
    bool is_buf_16u;
};

struct FeatureValOnlyPrecalc : ParallelLoopBody
{
    FeatureValOnlyPrecalc( const CascadeDataStorage* _storage, Mat* _valCache, int _sample_count )
    {
        storage = _storage;
        valCache = _valCache;
        sample_count = _sample_count;
    }
    void operator()( const Range& range ) const
    {
        for ( int fi = range.start; fi < range.end; fi++)
            for( int si = 0; si < sample_count; si++ )
                valCache->at<float>(fi,si) = storage->response( fi, si );
    }
    const CascadeDataStorage* storage;
    Mat* valCache;
    int sample_count;
};

void CascadeBoostTrainData::precalculate()
{
    int minNum = MIN( numPrecalcVal, numPrecalcIdx);

    QTime start; start.start();
    parallel_for_( Range(numPrecalcVal, numPrecalcIdx),
                   FeatureIdxOnlyPrecalc(storage, buf, sample_count, is_buf_16u!=0) );
    parallel_for_( Range(0, minNum),
                   FeatureValAndIdxPrecalc(storage, buf, &valCache, sample_count, is_buf_16u!=0) );
    parallel_for_( Range(minNum, numPrecalcVal),
                   FeatureValOnlyPrecalc(storage, &valCache, sample_count) );
    cout << "Precalculation time: " << (float)start.elapsed() / 1000 << " seconds" << endl;
}

//-------------------------------- CascadeBoostTree ----------------------------------------

CvDTreeNode* CascadeBoostTree::predict( int sampleIdx ) const
{
    CvDTreeNode* node = root;
    if( !node )
        CV_Error( CV_StsError, "The tree has not been trained yet" );

    if ( maxCatCount == 0 ) // ordered
    {
        while( node->left )
        {
            CvDTreeSplit* split = node->split;
            float val = ((CascadeBoostTrainData*)data)->getVarValue( split->var_idx, sampleIdx );
            node = val <= split->ord.c ? node->left : node->right;
        }
    }
    else // categorical
    {
        while( node->left )
        {
            CvDTreeSplit* split = node->split;
            int c = (int)((CascadeBoostTrainData*)data)->getVarValue( split->var_idx, sampleIdx );
            node = CV_DTREE_CAT_DIR(c, split->subset) < 0 ? node->left : node->right;
        }
    }
    return node;
}

void CascadeBoostTree::write( FileStorage &fs )
{
    int subsetN = (maxCatCount + 31)/32;
    queue<CvDTreeNode*> internalNodesQueue;
    int size = (int)pow( 2.f, (float)ensemble->get_params().max_depth);
    Ptr<float> leafVals = new float[size];
    int leafValIdx = 0;
    int internalNodeIdx = 1;
    CvDTreeNode* tempNode;

    CV_DbgAssert( root );
    internalNodesQueue.push( root );

    fs << "{";
    fs << CC_INTERNAL_NODES << "[:";
    while (!internalNodesQueue.empty())
    {
        tempNode = internalNodesQueue.front();
        CV_Assert( tempNode->left );
        if ( !tempNode->left->left && !tempNode->left->right) // left node is leaf
        {
            leafVals[-leafValIdx] = (float)tempNode->left->value;
            fs << leafValIdx-- ;
        }
        else
        {
            internalNodesQueue.push( tempNode->left );
            fs << internalNodeIdx++;
        }
        CV_Assert( tempNode->right );
        if ( !tempNode->right->left && !tempNode->right->right) // right node is leaf
        {
            leafVals[-leafValIdx] = (float)tempNode->right->value;
            fs << leafValIdx--;
        }
        else
        {
            internalNodesQueue.push( tempNode->right );
            fs << internalNodeIdx++;
        }
        int fidx = tempNode->split->var_idx;
        fs << fidx;
        if ( !maxCatCount )
            fs << tempNode->split->ord.c;
        else
            for( int i = 0; i < subsetN; i++ )
                fs << tempNode->split->subset[i];
        internalNodesQueue.pop();
    }
    fs << "]"; // CC_INTERNAL_NODES

    fs << CC_LEAF_VALUES << "[:";
    for (int ni = 0; ni < -leafValIdx; ni++)
        fs << leafVals[ni];
    fs << "]"; // CC_LEAF_VALUES
    fs << "}";
}

void CascadeBoostTree::read( const FileNode &node, CvBoost* _ensemble,
                                CvDTreeTrainData* _data )
{
    int subsetN = (maxCatCount + 31)/32;
    int step = 3 + ( maxCatCount>0 ? subsetN : 1 );

    queue<CvDTreeNode*> internalNodesQueue;
    FileNodeIterator internalNodesIt, leafValsuesIt;
    CvDTreeNode* prntNode, *cldNode;

    clear();
    data = _data;
    ensemble = _ensemble;
    pruned_tree_idx = 0;

    // read tree nodes
    FileNode rnode = node[CC_INTERNAL_NODES];
    internalNodesIt = rnode.end();
    leafValsuesIt = node[CC_LEAF_VALUES].end();
    internalNodesIt--; leafValsuesIt--;
    for( size_t i = 0; i < rnode.size()/step; i++ )
    {
        prntNode = data->new_node( 0, 0, 0, 0 );
        if ( maxCatCount > 0 )
        {
            prntNode->split = data->new_split_cat( 0, 0 );
            for( int j = subsetN-1; j>=0; j--)
            {
                *internalNodesIt >> prntNode->split->subset[j]; internalNodesIt--;
            }
        }
        else
        {
            float split_value;
            *internalNodesIt >> split_value; internalNodesIt--;
            prntNode->split = data->new_split_ord( 0, split_value, 0, 0, 0);
        }
        *internalNodesIt >> prntNode->split->var_idx; internalNodesIt--;
        int ridx, lidx;
        *internalNodesIt >> ridx; internalNodesIt--;
        *internalNodesIt >> lidx;internalNodesIt--;
        if ( ridx <= 0)
        {
            prntNode->right = cldNode = data->new_node( 0, 0, 0, 0 );
            *leafValsuesIt >> cldNode->value; leafValsuesIt--;
            cldNode->parent = prntNode;
        }
        else
        {
            prntNode->right = internalNodesQueue.front();
            prntNode->right->parent = prntNode;
            internalNodesQueue.pop();
        }

        if ( lidx <= 0)
        {
            prntNode->left = cldNode = data->new_node( 0, 0, 0, 0 );
            *leafValsuesIt >> cldNode->value; leafValsuesIt--;
            cldNode->parent = prntNode;
        }
        else
        {
            prntNode->left = internalNodesQueue.front();
            prntNode->left->parent = prntNode;
            internalNodesQueue.pop();
        }

        internalNodesQueue.push( prntNode );
    }

    root = internalNodesQueue.front();
    internalNodesQueue.pop();
}

void CascadeBoostTree::split_node_data( CvDTreeNode* node )
{
    int n = node->sample_count, nl, nr, scount = data->sample_count;
    char* dir = (char*)data->direction->data.ptr;
    CvDTreeNode *left = 0, *right = 0;
    int* newIdx = data->split_buf->data.i;
    int newBufIdx = data->get_child_buf_idx( node );
    int workVarCount = data->get_work_var_count();
    CvMat* buf = data->buf;
    size_t length_buf_row = data->get_length_subbuf();
    cv::AutoBuffer<uchar> inn_buf(n*(3*sizeof(int)+sizeof(float)));
    int* tempBuf = (int*)(uchar*)inn_buf;
    bool splitInputData;

    complete_node_dir(node);

    for( int i = nl = nr = 0; i < n; i++ )
    {
        int d = dir[i];
        // initialize new indices for splitting ordered variables
        newIdx[i] = (nl & (d-1)) | (nr & -d); // d ? ri : li
        nr += d;
        nl += d^1;
    }

    node->left = left = data->new_node( node, nl, newBufIdx, node->offset );
    node->right = right = data->new_node( node, nr, newBufIdx, node->offset + nl );

    splitInputData = node->depth + 1 < data->params.max_depth &&
        (node->left->sample_count > data->params.min_sample_count ||
        node->right->sample_count > data->params.min_sample_count);

    // split ordered variables, keep both halves sorted.
    for( int vi = 0; vi < ((CascadeBoostTrainData*)data)->numPrecalcIdx; vi++ )
    {
        int ci = data->get_var_type(vi);
        if( ci >= 0 || !splitInputData )
            continue;

        int n1 = node->get_num_valid(vi);
        float *src_val_buf = (float*)(tempBuf + n);
        int *src_sorted_idx_buf = (int*)(src_val_buf + n);
        int *src_sample_idx_buf = src_sorted_idx_buf + n;
        const int* src_sorted_idx = 0;
        const float* src_val = 0;
        data->get_ord_var_data(node, vi, src_val_buf, src_sorted_idx_buf, &src_val, &src_sorted_idx, src_sample_idx_buf);

        for(int i = 0; i < n; i++)
            tempBuf[i] = src_sorted_idx[i];

        if (data->is_buf_16u)
        {
            ushort *ldst, *rdst;
            ldst = (ushort*)(buf->data.s + left->buf_idx*length_buf_row +
                vi*scount + left->offset);
            rdst = (ushort*)(ldst + nl);

            // split sorted
            for( int i = 0; i < n1; i++ )
            {
                int idx = tempBuf[i];
                int d = dir[idx];
                idx = newIdx[idx];
                if (d)
                {
                    *rdst = (ushort)idx;
                    rdst++;
                }
                else
                {
                    *ldst = (ushort)idx;
                    ldst++;
                }
            }
            CV_Assert( n1 == n );
        }
        else
        {
            int *ldst, *rdst;
            ldst = buf->data.i + left->buf_idx*length_buf_row +
                vi*scount + left->offset;
            rdst = buf->data.i + right->buf_idx*length_buf_row +
                vi*scount + right->offset;

            // split sorted
            for( int i = 0; i < n1; i++ )
            {
                int idx = tempBuf[i];
                int d = dir[idx];
                idx = newIdx[idx];
                if (d)
                {
                    *rdst = idx;
                    rdst++;
                }
                else
                {
                    *ldst = idx;
                    ldst++;
                }
            }
            CV_Assert( n1 == n );
        }
    }

    // split cv_labels using newIdx relocation table
    int *src_lbls_buf = tempBuf + n;
    const int* src_lbls = data->get_cv_labels(node, src_lbls_buf);

    for(int i = 0; i < n; i++)
        tempBuf[i] = src_lbls[i];

    if (data->is_buf_16u)
    {
        unsigned short *ldst = (unsigned short *)(buf->data.s + left->buf_idx*length_buf_row +
            (workVarCount-1)*scount + left->offset);
        unsigned short *rdst = (unsigned short *)(buf->data.s + right->buf_idx*length_buf_row +
            (workVarCount-1)*scount + right->offset);

        for( int i = 0; i < n; i++ )
        {
            int idx = tempBuf[i];
            if (dir[i])
            {
                *rdst = (unsigned short)idx;
                rdst++;
            }
            else
            {
                *ldst = (unsigned short)idx;
                ldst++;
            }
        }

    }
    else
    {
        int *ldst = buf->data.i + left->buf_idx*length_buf_row +
            (workVarCount-1)*scount + left->offset;
        int *rdst = buf->data.i + right->buf_idx*length_buf_row +
            (workVarCount-1)*scount + right->offset;

        for( int i = 0; i < n; i++ )
        {
            int idx = tempBuf[i];
            if (dir[i])
            {
                *rdst = idx;
                rdst++;
            }
            else
            {
                *ldst = idx;
                ldst++;
            }
        }
    }

    // split sample indices
    int *sampleIdx_src_buf = tempBuf + n;
    const int* sampleIdx_src = data->get_sample_indices(node, sampleIdx_src_buf);

    for(int i = 0; i < n; i++)
        tempBuf[i] = sampleIdx_src[i];

    if (data->is_buf_16u)
    {
        unsigned short* ldst = (unsigned short*)(buf->data.s + left->buf_idx*length_buf_row +
            workVarCount*scount + left->offset);
        unsigned short* rdst = (unsigned short*)(buf->data.s + right->buf_idx*length_buf_row +
            workVarCount*scount + right->offset);
        for (int i = 0; i < n; i++)
        {
            unsigned short idx = (unsigned short)tempBuf[i];
            if (dir[i])
            {
                *rdst = idx;
                rdst++;
            }
            else
            {
                *ldst = idx;
                ldst++;
            }
        }
    }
    else
    {
        int* ldst = buf->data.i + left->buf_idx*length_buf_row +
            workVarCount*scount + left->offset;
        int* rdst = buf->data.i + right->buf_idx*length_buf_row +
            workVarCount*scount + right->offset;
        for (int i = 0; i < n; i++)
        {
            int idx = tempBuf[i];
            if (dir[i])
            {
                *rdst = idx;
                rdst++;
            }
            else
            {
                *ldst = idx;
                ldst++;
            }
        }
    }

    for( int vi = 0; vi < data->var_count; vi++ )
    {
        left->set_num_valid(vi, (int)(nl));
        right->set_num_valid(vi, (int)(nr));
    }

    // deallocate the parent node data that is not needed anymore
    data->free_node_data(node);
}

//----------------------------------- CascadeBoost --------------------------------------

bool CascadeBoost::train( const CascadeDataStorage* _storage,
                           int _numSamples,
                           int _precalcValBufSize, int _precalcIdxBufSize,
                           const CascadeBoostParams& _params )
{
    bool isTrained = false;
    CV_Assert( !data );
    clear();

    data = new CascadeBoostTrainData( _storage, _numSamples,
                                        _precalcValBufSize, _precalcIdxBufSize, _params );

    CvMemStorage *memStorage = cvCreateMemStorage();
    weak = cvCreateSeq( 0, sizeof(CvSeq), sizeof(CvBoostTree*), memStorage );
    memStorage = 0;

    set_params( _params );
    if ( (_params.boost_type == LOGIT) || (_params.boost_type == GENTLE) )
        data->do_responses_copy();

    update_weights( 0 );

    cout << "+----+---------+---------+" << endl;
    cout << "|  N |    HR   |    FA   |" << endl;
    cout << "+----+---------+---------+" << endl;

    do
    {
        CascadeBoostTree* tree = new CascadeBoostTree(_params.maxCatCount);
        if( !tree->train( data, subsample_mask, this ) )
        {
            delete tree;
            break;
        }

        cvSeqPush( weak, &tree );
        update_weights( tree );
        trim_weights();
        if( cvCountNonZero(subsample_mask) == 0 )
            break;
    }
    while( !isErrDesired() && (weak->total < params.weak_count) );

    if(weak->total > 0)
    {
        data->is_classifier = true;
        data->free_train_data();
        isTrained = true;
    }
    else
        clear();

    return isTrained;
}

float CascadeBoost::predict( int sampleIdx, bool applyThreshold ) const
{
    CV_Assert( weak );
    double sum = 0;
    CvSeqReader reader;
    cvStartReadSeq( weak, &reader );
    cvSetSeqReaderPos( &reader, 0 );
    for( int i = 0; i < weak->total; i++ )
    {
        CvBoostTree* wtree;
        CV_READ_SEQ_ELEM( wtree, reader );
        sum += ((CascadeBoostTree*)wtree)->predict(sampleIdx)->value;
    }
    if (applyThreshold)
        return (float)sum - threshold;
    return (float)sum;
}

bool CascadeBoost::set_params( const CvBoostParams& _params )
{
    minTAR = ((CascadeBoostParams&)_params).minTAR;
    maxFAR = ((CascadeBoostParams&)_params).maxFAR;
    return ( ( minTAR > 0 ) && ( minTAR < 1) &&
        ( maxFAR > 0 ) && ( maxFAR < 1) &&
        CvBoost::set_params( _params ));
}

void CascadeBoost::update_weights( CvBoostTree* tree )
{
    int n = data->sample_count;
    double sumW = 0.;
    int step = 0;
    float* fdata = 0;
    int *sampleIdxBuf;
    const int* sampleIdx = 0;
    int inn_buf_size = ((params.boost_type == LOGIT) || (params.boost_type == GENTLE) ? n*sizeof(int) : 0) +
                       ( !tree ? n*sizeof(int) : 0 );
    cv::AutoBuffer<uchar> inn_buf(inn_buf_size);
    uchar* cur_inn_buf_pos = (uchar*)inn_buf;
    if ( (params.boost_type == LOGIT) || (params.boost_type == GENTLE) )
    {
        step = CV_IS_MAT_CONT(data->responses_copy->type) ?
            1 : data->responses_copy->step / CV_ELEM_SIZE(data->responses_copy->type);
        fdata = data->responses_copy->data.fl;
        sampleIdxBuf = (int*)cur_inn_buf_pos; cur_inn_buf_pos = (uchar*)(sampleIdxBuf + n);
        sampleIdx = data->get_sample_indices( data->data_root, sampleIdxBuf );
    }
    CvMat* buf = data->buf;
    size_t length_buf_row = data->get_length_subbuf();
    if( !tree ) // before training the first tree, initialize weights and other parameters
    {
        int* classLabelsBuf = (int*)cur_inn_buf_pos; cur_inn_buf_pos = (uchar*)(classLabelsBuf + n);
        const int* classLabels = data->get_class_labels(data->data_root, classLabelsBuf);
        // in case of logitboost and gentle adaboost each weak tree is a regression tree,
        // so we need to convert class labels to floating-point values
        double w0 = 1./n;
        double p[2] = { 1, 1 };

        cvReleaseMat( &orig_response );
        cvReleaseMat( &sum_response );
        cvReleaseMat( &weak_eval );
        cvReleaseMat( &subsample_mask );
        cvReleaseMat( &weights );

        orig_response = cvCreateMat( 1, n, CV_32S );
        weak_eval = cvCreateMat( 1, n, CV_64F );
        subsample_mask = cvCreateMat( 1, n, CV_8U );
        weights = cvCreateMat( 1, n, CV_64F );
        subtree_weights = cvCreateMat( 1, n + 2, CV_64F );

        if (data->is_buf_16u)
        {
            unsigned short* labels = (unsigned short*)(buf->data.s + data->data_root->buf_idx*length_buf_row +
                data->data_root->offset + (data->work_var_count-1)*data->sample_count);
            for( int i = 0; i < n; i++ )
            {
                // save original categorical responses {0,1}, convert them to {-1,1}
                orig_response->data.i[i] = classLabels[i]*2 - 1;
                // make all the samples active at start.
                // later, in trim_weights() deactivate/reactive again some, if need
                subsample_mask->data.ptr[i] = (uchar)1;
                // make all the initial weights the same.
                weights->data.db[i] = w0*p[classLabels[i]];
                // set the labels to find (from within weak tree learning proc)
                // the particular sample weight, and where to store the response.
                labels[i] = (unsigned short)i;
            }
        }
        else
        {
            int* labels = buf->data.i + data->data_root->buf_idx*length_buf_row +
                data->data_root->offset + (data->work_var_count-1)*data->sample_count;

            for( int i = 0; i < n; i++ )
            {
                // save original categorical responses {0,1}, convert them to {-1,1}
                orig_response->data.i[i] = classLabels[i]*2 - 1;
                subsample_mask->data.ptr[i] = (uchar)1;
                weights->data.db[i] = w0*p[classLabels[i]];
                labels[i] = i;
            }
        }

        if( params.boost_type == LOGIT )
        {
            sum_response = cvCreateMat( 1, n, CV_64F );

            for( int i = 0; i < n; i++ )
            {
                sum_response->data.db[i] = 0;
                fdata[sampleIdx[i]*step] = orig_response->data.i[i] > 0 ? 2.f : -2.f;
            }

            // in case of logitboost each weak tree is a regression tree.
            // the target function values are recalculated for each of the trees
            data->is_classifier = false;
        }
        else if( params.boost_type == GENTLE )
        {
            for( int i = 0; i < n; i++ )
                fdata[sampleIdx[i]*step] = (float)orig_response->data.i[i];

            data->is_classifier = false;
        }
    }
    else
    {
        // at this moment, for all the samples that participated in the training of the most
        // recent weak classifier we know the responses. For other samples we need to compute them
        if( have_subsample )
        {
            // invert the subsample mask
            cvXorS( subsample_mask, cvScalar(1.), subsample_mask );

            // run tree through all the non-processed samples
            for( int i = 0; i < n; i++ )
                if( subsample_mask->data.ptr[i] )
                {
                    weak_eval->data.db[i] = ((CascadeBoostTree*)tree)->predict( i )->value;
                }
        }

        // now update weights and other parameters for each type of boosting
        if( params.boost_type == DISCRETE )
        {
            // Discrete AdaBoost:
            //   weak_eval[i] (=f(x_i)) is in {-1,1}
            //   err = sum(w_i*(f(x_i) != y_i))/sum(w_i)
            //   C = log((1-err)/err)
            //   w_i *= exp(C*(f(x_i) != y_i))

            double C, err = 0.;
            double scale[] = { 1., 0. };

            for( int i = 0; i < n; i++ )
            {
                double w = weights->data.db[i];
                sumW += w;
                err += w*(weak_eval->data.db[i] != orig_response->data.i[i]);
            }

            if( sumW != 0 )
                err /= sumW;
            C = err = -logRatio( err );
            scale[1] = exp(err);

            sumW = 0;
            for( int i = 0; i < n; i++ )
            {
                double w = weights->data.db[i]*
                    scale[weak_eval->data.db[i] != orig_response->data.i[i]];
                sumW += w;
                weights->data.db[i] = w;
            }

            tree->scale( C );
        }
        else if( params.boost_type == REAL )
        {
            // Real AdaBoost:
            //   weak_eval[i] = f(x_i) = 0.5*log(p(x_i)/(1-p(x_i))), p(x_i)=P(y=1|x_i)
            //   w_i *= exp(-y_i*f(x_i))

            for( int i = 0; i < n; i++ )
                weak_eval->data.db[i] *= -orig_response->data.i[i];

            cvExp( weak_eval, weak_eval );

            for( int i = 0; i < n; i++ )
            {
                double w = weights->data.db[i]*weak_eval->data.db[i];
                sumW += w;
                weights->data.db[i] = w;
            }
        }
        else if( params.boost_type == LOGIT )
        {
            // LogitBoost:
            //   weak_eval[i] = f(x_i) in [-z_max,z_max]
            //   sum_response = F(x_i).
            //   F(x_i) += 0.5*f(x_i)
            //   p(x_i) = exp(F(x_i))/(exp(F(x_i)) + exp(-F(x_i))=1/(1+exp(-2*F(x_i)))
            //   reuse weak_eval: weak_eval[i] <- p(x_i)
            //   w_i = p(x_i)*1(1 - p(x_i))
            //   z_i = ((y_i+1)/2 - p(x_i))/(p(x_i)*(1 - p(x_i)))
            //   store z_i to the data->data_root as the new target responses

            const double lbWeightThresh = FLT_EPSILON;
            const double lbZMax = 10.;

            for( int i = 0; i < n; i++ )
            {
                double s = sum_response->data.db[i] + 0.5*weak_eval->data.db[i];
                sum_response->data.db[i] = s;
                weak_eval->data.db[i] = -2*s;
            }

            cvExp( weak_eval, weak_eval );

            for( int i = 0; i < n; i++ )
            {
                double p = 1./(1. + weak_eval->data.db[i]);
                double w = p*(1 - p), z;
                w = MAX( w, lbWeightThresh );
                weights->data.db[i] = w;
                sumW += w;
                if( orig_response->data.i[i] > 0 )
                {
                    z = 1./p;
                    fdata[sampleIdx[i]*step] = (float)min(z, lbZMax);
                }
                else
                {
                    z = 1./(1-p);
                    fdata[sampleIdx[i]*step] = (float)-min(z, lbZMax);
                }
            }
        }
        else
        {
            // Gentle AdaBoost:
            //   weak_eval[i] = f(x_i) in [-1,1]
            //   w_i *= exp(-y_i*f(x_i))
            assert( params.boost_type == GENTLE );

            for( int i = 0; i < n; i++ )
                weak_eval->data.db[i] *= -orig_response->data.i[i];

            cvExp( weak_eval, weak_eval );

            for( int i = 0; i < n; i++ )
            {
                double w = weights->data.db[i] * weak_eval->data.db[i];
                weights->data.db[i] = w;
                sumW += w;
            }
        }
    }

    // renormalize weights
    if( sumW > FLT_EPSILON )
    {
        sumW = 1./sumW;
        for( int i = 0; i < n; ++i )
            weights->data.db[i] *= sumW;
    }
}

bool CascadeBoost::isErrDesired()
{
    int sampleCount = data->sample_count;
    QList<float> responses;

    for( int i = 0; i < sampleCount; i++ )
        if( ((CascadeBoostTrainData*)data)->storage->label(i) == 1.0F )
            responses.append(predict(i, false));
    sort(responses.begin(), responses.end());

    int numPos = responses.size(), numNeg = sampleCount - numPos;

    int thresholdIdx = (int)((1.0F - minTAR) * numPos);
    threshold = responses[thresholdIdx];

    int numTrueAccepts = numPos - thresholdIdx;
    for( int i = thresholdIdx - 1; i >= 0; i--) // add pos values lower than the threshold that have the same response
        if ( responses[i] - threshold > -FLT_EPSILON )
            numTrueAccepts++;
    float TAR = ((float) numTrueAccepts) / ((float) numPos);

    int numFalseAccepts = 0;
    for( int i = 0; i < sampleCount; i++ )
        if( ((CascadeBoostTrainData*)data)->storage->label(i) == 0.0F )
            if( predict(i, true) > -FLT_EPSILON)
                numFalseAccepts++;
    float FAR = ((float) numFalseAccepts) / ((float) numNeg);

    cout << "|"; cout.width(4); cout << right << weak->total;
    cout << "|"; cout.width(9); cout << right << TAR;
    cout << "|"; cout.width(9); cout << right << FAR;
    cout << "|" << endl;
    cout << "+----+---------+---------+" << endl;

    return FAR <= maxFAR;
}

void CascadeBoost::save(const char *filename) const
{
    FileStorage fs(filename, FileStorage::WRITE);
    write( fs );
}

void CascadeBoost::write( FileStorage &fs ) const
{
    CascadeBoostTree* weakTree;
    fs << CC_BOOST << "{";
    fs << CC_WEAK_COUNT << weak->total;
    fs << CC_STAGE_THRESHOLD << threshold;
    fs << CC_WEAK_CLASSIFIERS << "[";
    for( int wi = 0; wi < weak->total; wi++)
    {
        weakTree = *((CascadeBoostTree**) cvGetSeqElem( weak, wi ));
        weakTree->write( fs );
    }
    fs << "]";
    fs << "}";
}

void CascadeBoost::load(const char *filename, const CascadeDataStorage *_storage, const CascadeBoostParams &_params)
{
    FileStorage fs(filename, FileStorage::READ);
    FileNode root = fs.getFirstTopLevelNode();
    read( root, _storage, _params );
    fs.release();
}

bool CascadeBoost::read( const FileNode &node, const CascadeDataStorage* _storage, const CascadeBoostParams& _params )
{
    CvMemStorage* memStorage;
    clear();
    data = new CascadeBoostTrainData( _storage, _params );
    set_params( _params );

    node[CC_STAGE_THRESHOLD] >> threshold;
    FileNode rnode = node[CC_WEAK_CLASSIFIERS];

    memStorage = cvCreateMemStorage();
    weak = cvCreateSeq( 0, sizeof(CvSeq), sizeof(CvBoostTree*), memStorage );
    for( FileNodeIterator it = rnode.begin(); it != rnode.end(); it++ )
    {
        CascadeBoostTree* tree = new CascadeBoostTree();
        tree->read( *it, this, data );
        cvSeqPush( weak, &tree );
    }
    return true;
}

}
