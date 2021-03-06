/*
All modification made by Intel Corporation: © 2016 Intel Corporation

All contributions by the University of California:
Copyright (c) 2014, 2015, The Regents of the University of California (Regents)
All rights reserved.

All other contributions:
Copyright (c) 2014, 2015, the respective contributors
All rights reserved.
For the list of contributors go to https://github.com/BVLC/caffe/blob/master/CONTRIBUTORS.md


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef MKLDNN_SUPPORTED
#include <algorithm>
#include <cfloat>
#include <vector>

#include "caffe/common.hpp"
#include "caffe/layer.hpp"
#include "caffe/layers/mkldnn_layers.hpp"
#include "caffe/syncedmem.hpp"
#include "caffe/util/math_functions.hpp"


namespace caffe {

template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom
                                            ,const vector<Blob<Dtype>*>& top)
{
    VLOG(1) << "MKLDNNPoolingLayer<Dtype>::LayerSetUp: " << this->layer_param_.name();

    Layer<Dtype>::LayerSetUp(bottom, top);
    PoolingParameter pool_param = this->layer_param_.pooling_param();

    if (pool_param.global_pooling()) {
        CHECK(!(pool_param.has_kernel_size() || pool_param.has_kernel_h() || pool_param.has_kernel_w()))
            << "With Global_pooling: true Filter size cannot specified";
    } else {
        CHECK(!pool_param.has_kernel_size() != !(pool_param.has_kernel_h() && pool_param.has_kernel_w()))
            << "Filter size is kernel_size OR kernel_h and kernel_w; not both";
        CHECK(pool_param.has_kernel_size() ||(pool_param.has_kernel_h() && pool_param.has_kernel_w()))
            << "For non-square filters both kernel_h and kernel_w are required.";
    }
    CHECK((!pool_param.has_pad() && pool_param.has_pad_h() && pool_param.has_pad_w())
            || (!pool_param.has_pad_h() && !pool_param.has_pad_w()))
        << "pad is pad OR pad_h and pad_w are required.";
    CHECK((!pool_param.has_stride() && pool_param.has_stride_h() && pool_param.has_stride_w())
            || (!pool_param.has_stride_h() && !pool_param.has_stride_w()))
        << "Stride is stride OR stride_h and stride_w are required.";

    global_pooling_ = pool_param.global_pooling();
    if (global_pooling_) {
        kernel_h_ = bottom[0]->height();
        kernel_w_ = bottom[0]->width();
    } else {
        if (pool_param.has_kernel_size()) {
            kernel_h_ = kernel_w_ = pool_param.kernel_size();
        } else {
            kernel_h_ = pool_param.kernel_h();
            kernel_w_ = pool_param.kernel_w();
        }
    }
    CHECK_GT(kernel_h_, 0) << "Filter dimensions cannot be zero.";
    CHECK_GT(kernel_w_, 0) << "Filter dimensions cannot be zero.";
    if (!pool_param.has_pad_h()) {
        pad_t_ = pad_b_ = pad_l_ = pad_r_ = pool_param.pad();
    } else {
        pad_t_ = pad_b_ = pool_param.pad_h();
        pad_l_ = pad_r_ = pool_param.pad_w();
    }
    if (!pool_param.has_stride_h()) {
        stride_h_ = stride_w_ = pool_param.stride();
    } else {
        stride_h_ = pool_param.stride_h();
        stride_w_ = pool_param.stride_w();
    }
    if (global_pooling_) {
        CHECK(pad_t_ == 0 && pad_l_ == 0 && stride_h_ == 1 && stride_w_ == 1)
            << "With Global_pooling: true; only pad = 0 and stride = 1";
    }
    if (pad_t_ != 0 || pad_l_ != 0) {
        CHECK(this->layer_param_.pooling_param().pool() == PoolingParameter_PoolMethod_AVE
        || this->layer_param_.pooling_param().pool() == PoolingParameter_PoolMethod_MAX)
        << "Padding implemented only for average and max pooling.";
        CHECK_LT(pad_t_, kernel_h_);
        CHECK_LT(pad_l_, kernel_w_);
    }

    height_out_ = static_cast<int>(ceil(static_cast<float>(
        bottom[0]->height() + pad_t_ + pad_b_ - kernel_h_) / stride_h_)) + 1;
    width_out_ = static_cast<int>(ceil(static_cast<float>(
        bottom[0]->width() + pad_r_ + pad_l_ - kernel_w_) / stride_w_)) + 1;

    if (pad_t_ || pad_b_ || pad_r_ || pad_l_) {
        // If we have padding, ensure that the last pooling starts strictly
        // inside the image (instead of at the padding); otherwise clip the last.
        if ((height_out_ - 1) * stride_h_ >= bottom[0]->height() + pad_t_) {
          --height_out_;
        }
        if ((width_out_ - 1) * stride_w_ >= bottom[0]->width() + pad_l_) {
          --width_out_;
        }
        CHECK_LT((height_out_ - 1) * stride_h_, bottom[0]->height() + pad_t_);
        CHECK_LT((width_out_ - 1) * stride_w_, bottom[0]->width() + pad_l_);
    }

    auto h = bottom[0]->height() + pad_t_;
    while (h + pad_b_ < stride_h_*(height_out_ - 1) + kernel_h_) pad_b_++;

    auto w = bottom[0]->width() + pad_l_;
    while (w + pad_r_ < stride_w_*(width_out_ - 1) + kernel_w_) pad_r_++;
}

template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom
                                        ,const vector<Blob<Dtype>*>& top)
{
    VLOG(1) << "MKLDNNPoolingLayer<Dtype>::Reshape: "  << this->layer_param_.name();

    num_ = bottom[0]->num();
    channels_ = bottom[0]->channels();
    height_ = bottom[0]->height();
    width_ = bottom[0]->width();

    CHECK_EQ(4, bottom[0]->num_axes()) << "Input must have 4 axes, "
        << "corresponding to (num, channels, height, width)";

    top[0]->Reshape(bottom[0]->num(), channels_, height_out_, width_out_);

    if (top.size() > 1) {
        (reinterpret_cast<Blob<uint32_t>* > (top[1]) )->Reshape(num_,
            channels_, height_out_, width_out_);
    }
    if (top.size() == 1) {
        max_idx_.Reshape(bottom[0]->num(), channels_, height_out_, width_out_);
    }
}

template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::InitPoolingFwd(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top)
{
    if (std::is_same<Dtype, double>::value)  NOT_IMPLEMENTED;

    auto propagation = this->phase_ == TEST ? prop_kind::forward_scoring : prop_kind::forward_training;

    algorithm pooling_algorithm;
    switch (this->layer_param_.pooling_param().pool()) {
    case PoolingParameter_PoolMethod_MAX:
        pooling_algorithm = algorithm::pooling_max;
        break;
    case PoolingParameter_PoolMethod_AVE:
        pooling_algorithm = algorithm::pooling_avg;
        break;
    case PoolingParameter_PoolMethod_STOCHASTIC:
        NOT_IMPLEMENTED;
        break;
    default:
        LOG(FATAL) << "Unknown pooling method.";
    }

    int32_t n = this->num_;
    int32_t c = this->channels_;
    int32_t ih = this->height_;
    int32_t iw = this->width_;
    int32_t oh = this->height_out_;
    int32_t ow = this->width_out_;

    int32_t kh = this->kernel_h_;
    int32_t kw = this->kernel_w_;

    int32_t sh = this->stride_h_;
    int32_t sw = this->stride_w_;

    int32_t pt = this->pad_t_;
    int32_t pb = this->pad_b_;
    int32_t pl = this->pad_l_;
    int32_t pr = this->pad_r_;

    bool bottom_data_is_prv = (const_cast<Dtype*>(bottom[0]->prv_data()) != NULL);

    engine cpu_engine = CpuEngine::Instance().get_engine();
    memory::data_type mpcsn = memory::data_type::f32;
    memory::dims bottom_tz = {n, c, ih, iw};
    memory::dims top_tz = {n, c, oh, ow};
    memory::format mfmt_nchw = memory::format::nchw;

    // ---- Initialize memory descriptors -------------
    typedef typename memory::primitive_desc MemPD; // short name for memory::primitive_desc

    memory::format cmfmt = mfmt_nchw;
    if (bottom_data_is_prv) {
        shared_ptr<MKLDNNMemoryDescriptor<Dtype, false> > mem_descr
            = get_mkldnn_prv_descriptor<Dtype, false>(bottom[0]);
        cmfmt = static_cast<memory::format>(mem_descr->prv_memory_pd()->desc().data.format);
    }
    shared_ptr<memory::desc> init_fwd_bottom_md(new memory::desc({bottom_tz}, mpcsn, cmfmt));
    shared_ptr<memory::desc> init_fwd_top_md(new memory::desc({top_tz}, mpcsn, cmfmt));

    shared_ptr<MemPD> usr_bottom_data_mpd(new MemPD({{bottom_tz}, mpcsn, mfmt_nchw}, cpu_engine));
    shared_ptr<MemPD> usr_top_data_mpd(new MemPD({{top_tz}, mpcsn, mfmt_nchw}, cpu_engine));
    // ---- Initialize pooling primitive descriptor -------------
    pooling_forward::desc poolingFwd_desc(propagation, pooling_algorithm, *init_fwd_bottom_md,*init_fwd_top_md
                                        , {sh, sw}, {kh, kw}, {pt, pl}, {pb, pr}, padding_kind::zero);
    // ---- Determining engine to use -----------------------
    std::string subengines = this->layer_param_.engine();
    if (subengines == "" || subengines == "MKLDNN")
      subengines = "MKLDNN:CPU";
    EngineParser ep(subengines);
    unsigned subEngineIndex = 0;
    for(; subEngineIndex < ep.getNumberOfSubEngines(); subEngineIndex++) {
      try {
        poolingFwd_pd.reset(new pooling_forward::primitive_desc(poolingFwd_desc,
                ep.getMKLDNNSubEngine(subEngineIndex)));
      }
      catch(...) {
        continue;
      }
      break;
    }

    CHECK(poolingFwd_pd);
    engine engine = ep.getMKLDNNSubEngine(subEngineIndex);

    // ---- Initialize remaining memory descriptors -------------
    shared_ptr<MemPD> prv_fwd_bottom_data_mpd;
    shared_ptr<MemPD> prv_fwd_top_data_mpd;
    if (bottom_data_is_prv) {
        prv_fwd_bottom_data_mpd.reset(new MemPD(*init_fwd_bottom_md, engine));
        prv_fwd_top_data_mpd.reset(new MemPD(*init_fwd_top_md, engine));
    }

    // ---- Create priv memory  ---------------------

    // We'll output the mask to top[1] if it's of size >1.
    uint32_t* mask = NULL;  // suppress warnings about uninitalized variables
    // We'll output the mask to top[1] if it's of size >1.
    const bool use_top_mask = top.size() > 1;
    mask = (use_top_mask) ?  reinterpret_cast<uint32_t*>(top[1]->mutable_cpu_data())
            : max_idx_.mutable_cpu_data();

    // ---  init primitive and prv_memory descriptors ----------------------
    fwd_bottom_data.reset(new MKLDNNData<Dtype>(usr_bottom_data_mpd, prv_fwd_bottom_data_mpd, bottom[0], this));
    fwd_bottom_data_primitive = fwd_bottom_data->create_input(false);

    fwd_top_data.reset(new MKLDNNData<Dtype>(usr_top_data_mpd, prv_fwd_top_data_mpd, top[0], this));
    fwd_top_data_memory = fwd_top_data->create_output_memory();

    if ( propagation == prop_kind::forward_training && pooling_algorithm != algorithm::pooling_avg) {
        indices_pd.reset(new MemPD(poolingFwd_pd->workspace_primitive_desc()));
        indices_memory.reset(new memory(*indices_pd, reinterpret_cast<void *>(mask)));
        poolingFwd.reset(new pooling_forward(*poolingFwd_pd, *fwd_bottom_data_primitive, *fwd_top_data_memory, *indices_memory));
    } else {
        poolingFwd.reset(new pooling_forward(*poolingFwd_pd, *fwd_bottom_data_primitive, *fwd_top_data_memory));
    }
    fwd_bottom_data->set_mkldnn_primitive(poolingFwd);
    fwd_top_data->set_mkldnn_primitive(poolingFwd);
}

// TODO(Yangqing): Is there a faster way to do pooling in the channel-first
// case?
template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom
                                            ,const vector<Blob<Dtype>*>& top)
{
    VLOG(1) << "MKLDNNPoolingLayer<Dtype>::Forward_cpu: " << this->layer_param_.name();
    if (NULL == poolingFwd_pd)
        InitPoolingFwd(bottom, top);
    // making reorders if needed.
    fwd_bottom_data->sync_before_read();
    // update top that head at prv
    fwd_top_data->sync_before_write();

    poolingFwd.submit();
}

template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::InitPoolingBwd(const vector<Blob<Dtype>*>& top
                                               , const vector<bool>& propagate_down
                                               , const vector<Blob<Dtype>*>& bottom)
{
    if (std::is_same<Dtype, double>::value)  NOT_IMPLEMENTED;

    algorithm pooling_algorithm;
    switch (this->layer_param_.pooling_param().pool()) {
    case PoolingParameter_PoolMethod_MAX:
        pooling_algorithm = algorithm::pooling_max;
        break;
    case PoolingParameter_PoolMethod_AVE:
        pooling_algorithm = algorithm::pooling_avg;
        break;
    case PoolingParameter_PoolMethod_STOCHASTIC:
        NOT_IMPLEMENTED;
        break;
    default:
        LOG(FATAL) << "Unknown pooling method.";
    }

    int32_t n = this->num_;
    int32_t c = this->channels_;
    int32_t ih = this->height_;
    int32_t iw = this->width_;
    int32_t oh = this->height_out_;
    int32_t ow = this->width_out_;

    int32_t kh = this->kernel_h_;
    int32_t kw = this->kernel_w_;

    int32_t sh = this->stride_h_;
    int32_t sw = this->stride_w_;

    int32_t pt = this->pad_t_;
    int32_t pb = this->pad_b_;

    int32_t pr = this->pad_r_;
    int32_t pl = this->pad_l_;
    
    bool top_diff_is_prv = (const_cast<Dtype*>(top[0]->prv_diff()) != NULL);

    engine cpu_engine = CpuEngine::Instance().get_engine();
    memory::data_type mpcsn = memory::data_type::f32;
    memory::dims bottom_tz = {n, c, ih, iw};
    memory::dims top_tz = {n, c, oh, ow};
    memory::format mfmt_nchw = memory::format::nchw;

    // ---- Initialize memory descriptors -------------
    typedef typename memory::primitive_desc MemPD; // short name for memory::primitive_desc

    memory::format bwd_cmfmt = mfmt_nchw;
    if (top_diff_is_prv) {
        shared_ptr<MKLDNNMemoryDescriptor<Dtype, true> > mem_descr
            = get_mkldnn_prv_descriptor<Dtype, true>(top[0]);
        bwd_cmfmt = static_cast<memory::format>(mem_descr->prv_memory_pd()->desc().data.format);
    }

    shared_ptr<memory::desc> init_bwd_bottom_md(new memory::desc({bottom_tz}, mpcsn, bwd_cmfmt));
    shared_ptr<memory::desc> init_bwd_top_md(new memory::desc({top_tz}, mpcsn, bwd_cmfmt));

    shared_ptr<MemPD> usr_bottom_data_mpd(new MemPD({{bottom_tz}, mpcsn, mfmt_nchw}, cpu_engine));
    shared_ptr<MemPD> usr_top_data_mpd(new MemPD({{top_tz}, mpcsn, mfmt_nchw}, cpu_engine));
    // ---- Initialize pooling primitive descriptor -------------
    pooling_backward::desc poolingBwd_desc(pooling_algorithm, *init_bwd_bottom_md,*init_bwd_top_md
                                        , {sh, sw}, {kh, kw}, {pt, pl}, {pb, pr}, padding_kind::zero);
    // ---- Determining engine to use -----------------------
    std::string subengines = this->layer_param_.engine();
    if (subengines == "" || subengines == "MKLDNN")
      subengines = "MKLDNN:CPU";
    EngineParser ep(subengines);
    unsigned subEngineIndex = 0;
    for(; subEngineIndex < ep.getNumberOfSubEngines(); subEngineIndex++) {
      try {
        poolingBwd_pd.reset(new pooling_backward::primitive_desc(poolingBwd_desc,
                ep.getMKLDNNSubEngine(subEngineIndex), *poolingFwd_pd));
      }
      catch(...) {
        continue;
      }
      break;
    }

    CHECK(poolingBwd_pd);
    engine engine = ep.getMKLDNNSubEngine(subEngineIndex);

    // ---- Initialize remaining memory descriptors -------------
    shared_ptr<MemPD> prv_bwd_bottom_diff_mpd, prv_bwd_top_diff_mpd;
    if (top_diff_is_prv) {
        prv_bwd_bottom_diff_mpd.reset(new MemPD(*init_bwd_bottom_md, engine));
        prv_bwd_top_diff_mpd.reset(new MemPD(*init_bwd_top_md, engine));
    }

    // ---  init primitive and prv_memory descriptors ----------------------
    bwd_bottom_diff.reset(new MKLDNNDiff<Dtype>(usr_bottom_data_mpd, prv_bwd_bottom_diff_mpd, bottom[0], this));
    bwd_bottom_diff->name = "bwd_bottom_diff_data   @ " + this->layer_param_.name();
    bwd_bottom_diff_memory = bwd_bottom_diff->create_output_memory();

    bwd_top_diff.reset(new MKLDNNDiff<Dtype>(usr_top_data_mpd, prv_bwd_top_diff_mpd, top[0], this));
    bwd_top_diff->name = "bwd_top_diff_data   @ " + this->layer_param_.name();
    bwd_top_diff_primitive = bwd_top_diff->create_input(false);

    if (pooling_algorithm != algorithm::pooling_avg)
        poolingBwd.reset(new pooling_backward(*poolingBwd_pd,
                    *bwd_top_diff_primitive, *indices_memory,
                    *bwd_bottom_diff_memory));
    else
        poolingBwd.reset(new pooling_backward(*poolingBwd_pd,
                    *bwd_top_diff_primitive, *bwd_bottom_diff_memory));
    bwd_bottom_diff->set_mkldnn_primitive(poolingBwd);
    bwd_top_diff->set_mkldnn_primitive(poolingBwd);
}

template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top
                                            , const vector<bool>& propagate_down
                                            , const vector<Blob<Dtype>*>& bottom)
{
    VLOG(1) << "MKLDNNPoolingLayer<Dtype>::Backward_cpu: " << this->layer_param_.name();
    if (!propagate_down[0]) {
        return;
    }
    if (NULL == poolingBwd_pd)
        InitPoolingBwd(top, propagate_down, bottom);
    
    bwd_top_diff->sync_before_read();
    bwd_bottom_diff->sync_before_write();

    poolingBwd.submit();  
}

#ifdef CPU_ONLY
STUB_GPU(MKLDNNPoolingLayer);
#else
template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom
                                            ,const vector<Blob<Dtype>*>& top)
{ NOT_IMPLEMENTED; }

template <typename Dtype>
void MKLDNNPoolingLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top
                                            ,const vector<bool>& propagate_down
                                            ,const vector<Blob<Dtype>*>& bottom)
{ NOT_IMPLEMENTED; }
#endif

INSTANTIATE_CLASS(MKLDNNPoolingLayer);
}  // namespace caffe
#endif  // #ifdef MKLDNN_SUPPORTED
