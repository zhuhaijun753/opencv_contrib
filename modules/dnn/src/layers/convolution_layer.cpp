/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2013, OpenCV Foundation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "../precomp.hpp"
#include "layers_common.hpp"
#include "op_halide.hpp"
#include "opencv2/core/hal/hal.hpp"
#include "opencv2/core/hal/intrin.hpp"
#include <iostream>

namespace cv
{
namespace dnn
{

class BaseConvolutionLayerImpl : public ConvolutionLayer
{
public:
    BaseConvolutionLayerImpl() {}

    virtual bool supportBackend(int backendId)
    {
        return backendId == DNN_BACKEND_DEFAULT ||
               backendId == DNN_BACKEND_HALIDE && haveHalide();
    }

    void finalize(const std::vector<Mat*> &inputs, std::vector<Mat> &outputs)
    {
        CV_Assert(inputs.size() > 0);

        CV_Assert(blobs.size() >= 1 && blobs.size() <= 2);
        CV_Assert(blobs[0].dims == 4 && blobs[0].size[3] == kernel.width && blobs[0].size[2] == kernel.height);

        const Mat &input = *inputs[0];
        CV_Assert(input.dims == 4 && (input.type() == CV_32F || input.type() == CV_64F));
        for (size_t i = 0; i < inputs.size(); i++)
        {
            CV_Assert(inputs[i]->type() == input.type());
            CV_Assert(inputs[i]->dims == 4 && inputs[i]->size[1] == input.size[1]);
            CV_Assert(inputs[i]->size[2] == input.size[2] && inputs[i]->size[3] == input.size[3]);
        }

        Size outSize = Size(outputs[0].size[3], outputs[0].size[2]);
        getConvPoolPaddings(Size(input.size[3], input.size[2]), outSize,
                kernel, stride, padMode, pad);
    }

    bool hasBias() const
    {
        return blobs.size() >= 2;
    }

    virtual MatShape computeColRowShape(const MatShape &inpShape, const MatShape &outShape) const = 0;
    bool is1x1() const
    {
        return (kernel.height == 1 && kernel.width == 1) &&
        (stride.height == 1 && stride.width == 1) &&
        (dilation.height == 1 && dilation.width == 1);
    }
    bool setActivation(const Ptr<ActivationLayer>& ) { return false; }

    virtual void applyHalideScheduler(Ptr<BackendNode>& node,
                                      const std::vector<Mat*> &inputs,
                                      const std::vector<Mat> &outputs) const
    {
#ifdef HAVE_HALIDE
        Halide::Var x("x"), y("y"), c("c"), n("n"), tile("tile"), yi("yi"), yo("yo"), co("co"), ci("ci");
        Halide::Func& top = node.dynamicCast<HalideBackendNode>()->funcs[1];
        Halide::Func& padded_input = node.dynamicCast<HalideBackendNode>()->funcs[0];

        int outW, outH, outC, outN;
        getCanonicalSize(outputs[0].size, &outW, &outH, &outC, &outN);

        if (outW == 1 || outH <= 2)
            return;

        if (is1x1() || outC <= 16)
            top.reorder(x, c, y)
               .split(y, yo, yi, 2)
               .fuse(yo, n, tile)
               .parallel(tile)
               .unroll(yi)
               .vectorize(x, outW >= 16 ? 16 : outW);
        else
            top.reorder(x, c, y)
               .split(y, yo, yi, 2)
               .split(c, co, ci, 16)
               .fuse(yo, co, tile).fuse(n, tile, tile)
               .parallel(tile)
               .unroll(yi)
               .vectorize(x, outW >= 16 ? 16 : outW);
        padded_input.compute_at(top, yi);
#endif  // HAVE_HALIDE
    }
};

//TODO: simultaneously convolution and bias addition for cache optimization
class ConvolutionLayerImpl : public BaseConvolutionLayerImpl
{
public:
    enum { VEC_ALIGN = 8, DFT_TYPE = CV_32F };
    Mat weightsMat;
    Ptr<ActivationLayer> activ;

    MatShape computeColRowShape(const MatShape &inpShape, const MatShape &outShape) const
    {
        Size out(outShape[3], outShape[2]);
        int inpGroupCn = blobs[0].size[1];
        int ksize = inpGroupCn * kernel.height * kernel.width;
        return shape(out.area(), ksize);
    }

    bool getMemoryShapes(const std::vector<MatShape> &inputs,
                         const int requiredOutputs,
                         std::vector<MatShape> &outputs,
                         std::vector<MatShape> &internals) const
    {
        CV_Assert(blobs.size() != 0);
        CV_Assert(!hasBias() || blobs[1].total() == (size_t)blobs[0].size[0]);
        CV_Assert(inputs.size() == (size_t)1);

        internals.clear();

        int inpCn = inputs[0][1];
        int inpH = inputs[0][2];
        int inpW = inputs[0][3];

        int outCn = blobs[0].size[0];
        Size out;

        if (padMode.empty())
        {
            out.height = (inpH + 2 * pad.height - (dilation.height * (kernel.height - 1) + 1)) / stride.height + 1;
            out.width = (inpW + 2 * pad.width - (dilation.width * (kernel.width - 1) + 1)) / stride.width + 1;
        }
        else
        {
            getConvPoolOutParams(Size(inpH, inpW), kernel, stride, padMode, out);
        }

        int ngroups = inpCn / blobs[0].size[1];
        CV_Assert(inpCn % ngroups == 0 && outCn % ngroups == 0);

        int dims[] = {inputs[0][0], outCn, out.height, out.width};
        outputs.resize(inputs.size(), shape(dims));

        return false;
    }

#if 0
    bool setActivation(const Ptr<ActivationLayer>& layer) { activ = layer; return true; }
#else
    bool setActivation(const Ptr<ActivationLayer>&) { return false; }
#endif

    virtual Ptr<BackendNode> initHalide(const std::vector<Ptr<BackendWrapper> > &inputs)
    {
#ifdef HAVE_HALIDE
        Halide::Buffer<float> inputBuffer = halideBuffer(inputs[0]);

        const int inpCn = inputBuffer.channels();
        const int outCn = blobs[0].size[0];
        const int inpGroupCn = blobs[0].size[1];
        const int group = inpCn / inpGroupCn;
        const int outGroupCn = outCn / group;

        Halide::Buffer<float> weights = wrapToHalideBuffer(blobs[0]);

        Halide::Var x("x"), y("y"), c("c"), n("n");
        Halide::Func top = (name.empty() ? Halide::Func() : Halide::Func(name));
        Halide::Func padded_input(name + "_constant_exterior");
        if (pad.width || pad.height)
        {
            Halide::Func bounded =
                Halide::BoundaryConditions::constant_exterior(inputBuffer, 0);
            padded_input(x, y, c, n) = bounded(x, y, c, n);
        }
        else
        {
            padded_input(x, y, c, n) = inputBuffer(x, y, c, n);
        }

        Halide::RDom r(0, kernel.width, 0, kernel.height, 0, inpGroupCn);

        Halide::Expr kc = r.z;
        if (group > 1)
        {
            int outCnBound = outGroupCn;
            int inpChBound = inpGroupCn;
            Halide::Expr shift = select(c < outCnBound, 0, inpChBound);
            for (int i = 2; i < group; ++i)
            {
                outCnBound += outGroupCn;
                inpChBound += inpGroupCn;
                shift = select(c < outCnBound, shift, inpChBound);
            }
            kc += shift;
        }

        Halide::Expr kx = x * stride.width - pad.width + r.x * dilation.width;
        Halide::Expr ky = y * stride.height - pad.height + r.y * dilation.height;
        Halide::Expr topExpr = sum(padded_input(kx, ky, kc, n) *
                                   weights(r.x, r.y, r.z, c));
        if (hasBias())
        {
            Halide::Buffer<float> bias = wrapToHalideBuffer(blobs[1], {outCn});
            topExpr += bias(c);
        }
        top(x, y, c, n) = topExpr;
        Ptr<BackendNode> pp(new HalideBackendNode({ padded_input, top }));
        return Ptr<BackendNode>(new HalideBackendNode({ padded_input, top }));
#endif  // HAVE_HALIDE
        return Ptr<BackendNode>();
    }

    class ParallelConv : public cv::ParallelLoopBody
    {
    public:
        enum { BLK_SIZE = 32, BLK_SIZE_CN = 64 };

        const Mat* input_;
        const Mat* weights_;
        Mat* output_;
        int outShape[4];
        Size kernel_, pad_, stride_, dilation_;
        int ngroups_, nstripes_;
        std::vector<int> ofstab_;
        std::vector<float> biasvec_;
        const ActivationLayer* activ_;
        bool is1x1_;
        bool useAVX2;

        ParallelConv() {}

        static void run( const Mat& input, Mat& output,
                         const Mat& weights, const Mat& bias,
                         Size kernel, Size pad, Size stride, Size dilation,
                         int ngroups, int nstripes, const ActivationLayer* activ )
        {
            CV_Assert( input.dims == 4 && output.dims == 4 &&
                       input.size[0] == output.size[0] &&
                       weights.rows == output.size[1] &&
                       weights.cols == (input.size[1]/ngroups)*kernel.width*kernel.height &&
                       input.type() == output.type() &&
                       input.type() == weights.type() &&
                       input.type() == CV_32F &&
                       input.isContinuous() &&
                       output.isContinuous() &&
                       (bias.empty() || (bias.isContinuous() && bias.type() == CV_32F &&
                                         bias.total() == (size_t)output.size[1])));
            ParallelConv p;

            p.input_ = &input;
            p.weights_ = &weights;
            p.output_ = &output;
            for( int i = 0; i < 4; i++ ) p.outShape[i] = output.size[i];
            p.outShape[1] /= ngroups;
            p.kernel_ = kernel; p.pad_ = pad; p.stride_ = stride; p.dilation_ = dilation;
            p.ngroups_ = ngroups;
            p.nstripes_ = nstripes;
            p.activ_ = activ;
            int inpCnAll = input.size[1], width = input.size[3], height = input.size[2];
            int inpCn = inpCnAll / ngroups;
            int k, outCn = output.size[1];
            p.is1x1_ = kernel == Size(0,0) && pad == Size(0, 0);
            p.useAVX2 = checkHardwareSupport(CPU_AVX2);

            int ncn = std::min(inpCn, (int)BLK_SIZE_CN);
            p.ofstab_.resize(kernel.width*kernel.height*ncn);
            int* ofstab = &p.ofstab_[0];

            for( k = 0; k < ncn; k++ )
                for( int k_r = 0; k_r < kernel.height; k_r++ )
                    for( int k_c = 0; k_c < kernel.width; k_c++ )
                        ofstab[(k*kernel.height + k_r)*kernel.width + k_c] =
                        (k*height + k_r*dilation.height)*width + k_c*dilation.width;

            p.biasvec_.resize(outCn+2);
            float* biasvec = &p.biasvec_[0];
            if( bias.empty() )
            {
                for( k = 0; k < outCn; k++ )
                    biasvec[k] = 0.f;
            }
            else
            {
                for( k = 0; k < outCn; k++ )
                    biasvec[k] = bias.at<float>(k);
            }
            biasvec[outCn] = biasvec[outCn+1] = biasvec[outCn-1];
            parallel_for_(Range(0, nstripes), p, nstripes);
        }

        virtual void operator ()(const Range &r0) const
        {
            const int valign = ConvolutionLayerImpl::VEC_ALIGN;
            int ngroups = ngroups_, batchSize = input_->size[0]*ngroups;
            int outW = output_->size[3], outH = output_->size[2], outCn = output_->size[1]/ngroups;
            int width = input_->size[3], height = input_->size[2], inpCn = input_->size[1]/ngroups;
            int nstripes = nstripes_;
            int kernel_w = kernel_.width, kernel_h = kernel_.height;
            int pad_w = pad_.width, pad_h = pad_.height;
            int stride_w = stride_.width, stride_h = stride_.height;
            int dilation_w = dilation_.width, dilation_h = dilation_.height;
            int karea = kernel_w*kernel_h;
            int i, j, k;
            size_t inpPlaneSize = width*height;
            size_t outPlaneSize = outW*outH;
            bool is1x1 = is1x1_;

            int stripesPerSample;
            size_t stripeSize;
            Range r = r0;

            if( nstripes >= batchSize*2 )
            {
                stripesPerSample = nstripes/batchSize;
                stripeSize = alignSize((outPlaneSize + stripesPerSample - 1)/stripesPerSample, valign);
                stripeSize = std::min(stripeSize, outPlaneSize);
            }
            else
            {
                stripesPerSample = 1;
                int samplesPerStripe = std::max((batchSize + nstripes - 1)/nstripes, 1);
                r.start *= samplesPerStripe;
                r.end *= samplesPerStripe;
                nstripes *= samplesPerStripe;
                stripeSize = outPlaneSize;
            }

            const float* data_inp0_ = input_->ptr<float>();
            const int* ofstab = &ofstab_[0];
            const float* wptr_orig_ = weights_->ptr<float>();
            size_t wstep = weights_->step1();
            const float* biasvec = &biasvec_[0];
            float* data_out0_ = output_->ptr<float>();
            size_t rowbufsz = (size_t)karea*BLK_SIZE_CN*BLK_SIZE;
            AutoBuffer<float> rowbuf0_(rowbufsz + valign);
            float* rowbuf0 = alignPtr((float*)rowbuf0_, (int)(valign*sizeof(float)));

            // we clear the buffer once; ultimately, it lets us to avoid
            // tail processing after running the unrolled/vectorized loop.
            // the main idea is to make sure that the tail (a.k.a. padding) of each row
            // (i.e. the elements with indices between vsz=karea*ncn and vsz_a)
            // does not contain NaNs or Infs. Because the padding in the weights
            // matrix is explicitly initialized with 0's, we handle all other
            // cases nicely, i.e. we can skip expliciting re-initialization
            // of the padding - we just retain elements from the previous iteration
            // of the loop over channels (cn0).
            memset(rowbuf0, 0, rowbufsz*sizeof(rowbuf0[0]) );

            for( int stripe = r.start; stripe < r.end; stripe++ )
            {
                int subsampleIdx = stripe/stripesPerSample;
                if( subsampleIdx >= batchSize )
                    break;
                int stripeStart = (int)((stripe - subsampleIdx*stripesPerSample)*stripeSize);
                int stripeEnd = (int)std::min(stripeStart + stripeSize, outPlaneSize);
                const float* data_inp0 = data_inp0_ + subsampleIdx*inpPlaneSize*inpCn;
                float* data_out0 = data_out0_ + subsampleIdx*outPlaneSize*outCn;
                int startOutCn = (subsampleIdx % ngroups)*outCn;
                const float* wptr_orig = wptr_orig_ + wstep*startOutCn;
                const float* biasptr = biasvec + startOutCn;

                for( int cn0 = 0; cn0 < inpCn; cn0 += BLK_SIZE_CN )
                {
                    int cn1 = std::min(cn0 + BLK_SIZE_CN, inpCn);
                    int ncn = cn1 - cn0, vsz = karea*ncn;
                    int vsz_a = (int)alignSize(vsz, valign);
                    const float* wptr = wptr_orig + cn0*karea;

                    for( int ofs0 = stripeStart; ofs0 < stripeEnd; ofs0 += BLK_SIZE )
                    {
                        int ofs, ofs1 = std::min(ofs0 + BLK_SIZE, stripeEnd);

                        // do im2row for a part of input tensor
                        if( is1x1 )
                        {
                            for( ofs = ofs0; ofs < ofs1; ofs++ )
                            {
                                int out_i = ofs / outW;
                                int out_j = ofs - out_i * outW;
                                float* rowbuf = rowbuf0 + (ofs - ofs0)*vsz_a;

                                int in_i = out_i * stride_h - pad_h;
                                int in_j = out_j * stride_w - pad_w;
                                const float* imgptr = data_inp0 + (cn0*height + in_i)*width + in_j;

                                for( k = 0; k < vsz; k++ )
                                    rowbuf[k] = imgptr[k*inpPlaneSize];
                            }
                        }
                        else
                        {
                            for( ofs = ofs0; ofs < ofs1; ofs++ )
                            {
                                int out_i = ofs / outW;
                                int out_j = ofs - out_i * outW;
                                float* rowbuf = rowbuf0 + (ofs - ofs0)*vsz_a;

                                int in_i = out_i * stride_h - pad_h;
                                int in_j = out_j * stride_w - pad_w;
                                const float* imgptr = data_inp0 + (cn0*height + in_i)*width + in_j;

                                // this condition should be true for most of the tensor elements, i.e.
                                // most of the time the kernel aperture is inside the tensor X-Y plane.
                                if( 0 <= in_i && in_i < height - (kernel_h-1)*dilation_h &&
                                    0 <= in_j && in_j < width - (kernel_w-1)*dilation_w )
                                {
                                    for( k = 0; k < vsz; k++ )
                                        rowbuf[k] = imgptr[ofstab[k]];
                                }
                                else
                                {
                                    int i0 = std::max(0, (-in_i + dilation_h-1)/dilation_h);
                                    int i1 = std::min(kernel_h, (height - in_i + dilation_h-1)/dilation_h);
                                    int j0 = std::max(0, (-in_j + dilation_w-1)/dilation_w);
                                    int j1 = std::min(kernel_w, (width - in_j + dilation_w-1)/dilation_w);

                                    // here some non-continous sub-row of the row will not be
                                    // filled from the tensor; we need to make sure that the uncovered
                                    // elements are explicitly set to 0's. the easiest way is to
                                    // set all the elements to 0's before the loop.
                                    memset(rowbuf, 0, vsz*sizeof(rowbuf[0]));
                                    for( k = 0; k < ncn; k++, imgptr += width*height )
                                    {
                                        for( i = i0; i < i1; i++ )
                                        {
                                            for( j = j0; j < j1; j++ )
                                            {
                                                int imgofs = i*(dilation_h*width) + j*dilation_w;
                                                rowbuf[(k*kernel_h + i)*kernel_w + j] = imgptr[imgofs];
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // now compute dot product of the weights
                        // and im2row-transformed part of the tensor
                        int bsz = ofs1 - ofs0;
                    #if CV_DNN_TRY_AVX2
                        if(useAVX2)
                            fastConv_avx2(wptr, wstep, biasptr, rowbuf0, data_out0 + ofs0, outShape, bsz, vsz, vsz_a, cn0 == 0);
                        else
                    #endif
                        for( int i = 0; i < outCn; i += 2 )
                        {
                            const float* wptr0 = wptr + i*wstep;
                            const float* wptr1 = wptr0 + wstep;
                            float* outptr0 = data_out0 + ofs0 + i*outPlaneSize;
                            float* outptr1 = outptr0 + outPlaneSize;
                            float bias0 = biasptr[i], bias1 = biasptr[i+1];

                            if( i+1 >= outCn )
                            {
                                wptr1 = wptr0;
                                outptr1 = outptr0;
                                bias1 = bias0;
                            }

                            int j = 0;
                        #if CV_SIMD128
                            for( ; j <= bsz - 4; j += 4 )
                            {
                                const float* rptr = rowbuf0 + j*vsz_a;
                                v_float32x4 s0, s1;

                                if( cn0 == 0 )
                                {
                                    s0 = v_setall_f32(bias0);
                                    s1 = v_setall_f32(bias1);
                                }
                                else
                                {
                                    s0 = v_load(outptr0 + j);
                                    s1 = v_load(outptr1 + j);
                                }

                                v_float32x4 vs00 = v_setzero_f32(), vs01 = v_setzero_f32(),
                                            vs02 = v_setzero_f32(), vs03 = v_setzero_f32(),
                                            vs10 = v_setzero_f32(), vs11 = v_setzero_f32(),
                                            vs12 = v_setzero_f32(), vs13 = v_setzero_f32();
                                for( k = 0; k < vsz; k += 4, rptr += 4 )
                                {
                                    v_float32x4 w0 = v_load_aligned(wptr0 + k), w1 = v_load_aligned(wptr1 + k);
                                    v_float32x4 r0 = v_load_aligned(rptr), r1 = v_load_aligned(rptr + vsz_a),
                                                r2 = v_load_aligned(rptr + vsz_a*2), r3 = v_load_aligned(rptr + vsz_a*3);

                                    vs00 += w0*r0;
                                    vs01 += w0*r1;
                                    vs02 += w0*r2;
                                    vs03 += w0*r3;

                                    vs10 += w1*r0;
                                    vs11 += w1*r1;
                                    vs12 += w1*r2;
                                    vs13 += w1*r3;
                                }
                                s0 += v_reduce_sum4(vs00, vs01, vs02, vs03);
                                s1 += v_reduce_sum4(vs10, vs11, vs12, vs13);

                                v_store(outptr0 + j, s0);
                                v_store(outptr1 + j, s1);
                            }
                        #endif
                            for( ; j < bsz; j++ )
                            {
                                const float* rptr = rowbuf0 + j*vsz_a;
                                float s00, s10;

                                if( cn0 == 0 )
                                {
                                    s00 = bias0;
                                    s10 = bias1;
                                }
                                else
                                {
                                    s00 = outptr0[j];
                                    s10 = outptr1[j];
                                }

                                for( k = 0; k < vsz; k++ )
                                {
                                    float r0 = rptr[k];
                                    s00 += wptr0[k]*r0;
                                    s10 += wptr1[k]*r0;
                                }

                                outptr0[j] = s00;
                                outptr1[j] = s10;
                            }
                        }
                    }
                }

                if( activ_ )
                    activ_->forwardSlice(data_out0 + stripeStart, data_out0 + stripeStart,
                                         (int)(stripeEnd - stripeStart),
                                         outPlaneSize, startOutCn, startOutCn + outCn);
            }
        }
    };

    class ParallelDFTWeights : ParallelLoopBody
    {
    public:
        const Mat* weights_;
        Mat* wspectrums_;
        int nstripes_;
        Size kernel_, dftsz_;
        int nouts_, ninps_;

        static void run(const Mat& weights, Mat& wspectrums, Size kernel, Size dftsz, int nstripes)
        {
            CV_Assert(weights.type() == DFT_TYPE);

            ParallelDFTWeights p;
            p.weights_ = &weights;
            p.wspectrums_ = &wspectrums;
            p.nstripes_ = nstripes;
            p.kernel_ = kernel;
            p.dftsz_ = dftsz;
            p.nouts_ = weights.rows;
            p.ninps_ = weights.cols / (kernel.area());
            int dft_total = dftsz.area();
            int sz[] = { p.nouts_, p.ninps_, dft_total };
            wspectrums.create(3, sz, DFT_TYPE);

            parallel_for_(Range(0, nstripes), p, nstripes);
        }

        ParallelDFTWeights() {}

        void operator()(const Range& r) const
        {
            int ninps = ninps_, nouts = nouts_;
            int totalDFTs = nouts*ninps;
            int stripeSize = (totalDFTs + nstripes_-1)/nstripes_;
            int stripeStart = r.start*stripeSize;
            int stripeEnd = std::min(r.end*stripeSize, totalDFTs);
            int kernel_w = kernel_.width, kernel_h = kernel_.height;
            int dft_w = dftsz_.width, dft_h = dftsz_.height;
            float* wptr = (float*)weights_->ptr<float>();
            size_t wstep = weights_->step1();
            Ptr<hal::DFT2D> dft2d_fwd = hal::DFT2D::create(dft_w, dft_h, DFT_TYPE, 1, 1, 0, kernel_h);

            for( int i = stripeStart; i < stripeEnd; i++ )
            {
                int out = i / ninps;
                int inp = i % ninps;
                float* srcptr = wptr + out*wstep + inp*kernel_w*kernel_h;
                Mat src(kernel_h, kernel_w, DFT_TYPE, srcptr);
                float* dstptr = wspectrums_->ptr<float>(out, inp);
                Mat dst(dft_h, dft_w, DFT_TYPE, dstptr);
                size_t dstep = dft_w*sizeof(dstptr[0]);
                memset(dstptr, 0, dstep*dft_h);
                for( int j = 0; j < kernel_h; j++ )
                    memcpy(dstptr + dft_w*j, srcptr + kernel_w*j, kernel_w*sizeof(dstptr[0]));

                dft2d_fwd->apply((uchar*)dstptr, dstep, (uchar*)dstptr, dstep);
            }
        }
    };

    /*class ParallelDFTConv : public ParallelLoopBody
    {
    public:
        enum { BLK_SIZE = 32, BLK_SIZE_CN = 64 };

        const Mat* input_;
        const Mat* weights_;
        Mat* output_;
        Mat wspectrums_;
        int outShape[4];
        Size kernel_, pad_, blksz_, dftsz_;
        int ngroups_, nstripes_;
        std::vector<float> biasvec_;
        const ActivationLayer* activ_;

        static void run( const Mat& input, Mat& output,
                         const Mat& weights, const Mat& bias,
                         Size kernel, Size pad, int ngroups, int nstripes,
                         const ActivationLayer* activ )
        {
            CV_Assert( input.dims == 4 && output.dims == 4 &&
                       input.size[0] == output.size[0] &&
                       weights.rows == output.size[1] &&
                       weights.cols == (input.size[1]/ngroups)*kernel.width*kernel.height &&
                       input.type() == output.type() &&
                       input.type() == weights.type() &&
                       input.type() == CV_32F &&
                       input.isContinuous() &&
                       output.isContinuous() &&
                       (bias.empty() || (bias.isContinuous() && bias.type() == CV_32F &&
                                         bias.total() == (size_t)output.size[1])));
            ParallelDFTConv p;

            p.input_ = &input;
            p.weights_ = &weights;
            p.output_ = &output;
            for( int i = 0; i < 4; i++ ) p.outShape[i] = output.size[i];
            p.outShape[1] /= ngroups;
            p.kernel_ = kernel; p.pad_ = pad;
            p.ngroups_ = ngroups;
            p.nstripes_ = nstripes;
            p.activ_ = activ;

            const double blockScale = 4.5;
            const int minBlockSize = 32;

            Size resultsz(output.size[3], output.size[2]);
            Size blksz, dftsz;

            blksz.width = cvRound(kernel.width*blockScale);
            blksz.width = std::max(blksz.width, minBlockSize - kernel.width + 1);
            blksz.width = std::min(blksz.width, resultsz.width);
            blksz.height = cvRound(kernel.height*blockScale);
            blksz.height = std::max(blksz.height, minBlockSize - kernel.height + 1);
            blksz.height = std::min(blksz.height, resultsz.height);

            // compute DFT size along each dimension; make sure it's even, because we want
            // real DFT & inverse DFT to be fast.
            dftsz.width = blksz.width + kernel.width - 1;
            for(;;)
            {
                dftsz.width = getOptimalDFTSize(dftsz.width);
                if( dftsz.width <= 0 )
                    CV_Error( CV_StsOutOfRange, "cannot compute the right DFT size" );
                if(dftsz.width % 2 == 0)
                    break;
                dftsz.width++;
            }
            dftsz.height = blksz.height + kernel.height - 1;
            for(;;)
            {
                dftsz.height = getOptimalDFTSize(dftsz.height);
                if( dftsz.height <= 0 )
                    CV_Error( CV_StsOutOfRange, "cannot compute the right DFT size" );
                if(dftsz.height % 2 == 0)
                    break;
            }

            // transform all the weights for the layer; we do it on each run because
            // if we compute and store spectrums of all the weights for all the convolution
            // layers, it may take a lot of memory
            ParallelDFTWeights::run(weights, p.wspectrums_, kernel, dftsz, nstripes);

            // recompute block size
            blksz.width = dftsz.width - kernel.width + 1;
            blksz.width = std::min(blksz.width, resultsz.width);
            blksz.height = dftsz.height - kernel.height + 1;
            blksz.height = std::min(blksz.height, resultsz.height);

            printf("DFT conv: blk=(%d x %d), DFT=(%d x %d)\n", blksz.width, blksz.height, dftsz.width, dftsz.height);

            p.dftsz_ = dftsz;
            p.blksz_ = blksz;

            int k, outCn = output.size[1];
            p.biasvec_.resize(outCn+2);
            float* biasvec = &p.biasvec_[0];
            if( bias.empty() )
            {
                for( k = 0; k < outCn; k++ )
                    biasvec[k] = 0.f;
            }
            else
            {
                for( k = 0; k < outCn; k++ )
                    biasvec[k] = bias.at<float>(k);
            }
            biasvec[outCn] = biasvec[outCn+1] = biasvec[outCn-1];
            parallel_for_(Range(0, nstripes), p, nstripes);
        }

        ParallelDFTConv() {}

        void operator()(const Range& r0) const
        {
            int ngroups = ngroups_, batchSize = input_->size[0]*ngroups;
            int out_w = output_->size[3], out_h = output_->size[2], outCn = output_->size[1]/ngroups;
            int width = input_->size[3], height = input_->size[2], inpCn = input_->size[1]/ngroups;
            int nstripes = nstripes_;
            int kernel_w = kernel_.width, kernel_h = kernel_.height;
            int pad_w = pad_.width, pad_h = pad_.height;
            int blk_w = blksz_.width, blk_h = blksz_.height;
            int dft_w = dftsz_.width, dft_h = dftsz_.height;
            int dft_elems = dft_w*dft_h;
            size_t dftstep = dft_w*sizeof(float);
            int i, j;
            size_t inpPlaneSize = width*height;
            size_t outPlaneSize = out_w*out_h;
            int ndfts_w = (out_w + blk_w - 1)/blk_w;
            int ndfts_h = (out_h + blk_h - 1)/blk_h;
            int ndfts_plane = ndfts_w*ndfts_h;

            int stripesPerSample;
            int ndfts_stripe;
            Range r = r0;

            if( nstripes >= batchSize*2 )
            {
                stripesPerSample = nstripes/batchSize;
                ndfts_stripe = (ndfts_plane + stripesPerSample - 1)/stripesPerSample;
            }
            else
            {
                stripesPerSample = 1;
                int samplesPerStripe = std::max((batchSize + nstripes - 1)/nstripes, 1);
                r.start *= samplesPerStripe;
                r.end *= samplesPerStripe;
                nstripes *= samplesPerStripe;
                ndfts_stripe = ndfts_plane;
            }

            Mat spectrums((inpCn+1)*dft_h, dft_w, DFT_TYPE);
            Mat out_spectrum = spectrums.rowRange(dft_h*inpCn, dft_h*(inpCn+1));
            const float* wptr0 = wspectrums_.ptr<float>();
            const float* data_inp0_ = input_->ptr<float>();
            const float* biasvec = &biasvec_[0];
            float* data_out0_ = output_->ptr<float>();
            float dft_scale = 1.f/(dft_w*dft_h);

            Ptr<hal::DFT2D> dft2d_fwd = hal::DFT2D::create(dft_w, dft_h, DFT_TYPE, 1, 1,
                                                           CV_HAL_DFT_IS_INPLACE, blk_h + kernel_h - 1);
            Ptr<hal::DFT2D> dft2d_inv = hal::DFT2D::create(dft_w, dft_h, DFT_TYPE, 1, 1,
                                        CV_HAL_DFT_INVERSE|CV_HAL_DFT_SCALE, blk_h);

            for( int stripe = r.start; stripe < r.end; stripe++ )
            {
                int subsampleIdx = stripe/stripesPerSample;
                if( subsampleIdx >= batchSize )
                    break;
                int startOutCn = (subsampleIdx % ngroups)*outCn;
                const float* biasptr = biasvec + startOutCn;
                int dft_idx0 = (stripe - subsampleIdx*stripesPerSample)*ndfts_stripe;
                int dft_idx1 = std::min(dft_idx0 + ndfts_stripe, ndfts_plane);

                for( int dft_idx = dft_idx0; dft_idx < dft_idx1; dft_idx++ )
                {
                    int dft_y = dft_idx / dft_w;
                    int dft_x = dft_idx - dft_y*dft_w;
                    dft_x *= blk_w;
                    dft_y *= blk_h;
                    int bw = std::min(blk_w, out_w - dft_x);
                    int bh = std::min(blk_h, out_h - dft_y);
                    int patch_w = bw + kernel_w - 1;
                    int patch_h = bh + kernel_h - 1;
                    int in_x = dft_x - pad_w;
                    int in_y = dft_y - pad_h;
                    int i0 = std::max(0, -in_y);
                    int i1 = std::min(patch_h, height - in_y);
                    int j0 = std::max(0, -in_x);
                    int j1 = std::min(patch_w, width - in_x);

                    const float* data_inp = data_inp0_ + subsampleIdx*inpPlaneSize*inpCn + in_y*width + in_x;
                    float* sdata0 = spectrums.ptr<float>();
                    float* data_out = data_out0_ + subsampleIdx*outPlaneSize*outCn + dft_y*out_w + dft_x;

                    // phase 1. extract tiles from the input tensor channels and
                    // compute their spectrums.
                    float* sdata = sdata0;
                    for( int cn = 0; cn < inpCn; cn++, data_inp += inpPlaneSize )
                    {
                        for( i = 0; i < dft_h; i++, sdata += dft_w )
                        {
                            if( i < i0 || i >= i1 )
                                memset(sdata, 0, dft_w*sizeof(sdata[0]));
                            else
                            {
                                for( j = 0; j < j0; j++ )
                                    sdata[j] = 0.f;
                                for( ; j < j1; j++ )
                                    sdata[j] = data_inp[i*width + j];
                                for( ; j < dft_w; j++ )
                                    sdata[j] = 0.f;
                            }
                        }
                        uchar* dftdata = (uchar*)(sdata - dft_elems);
                        dft2d_fwd->apply(dftdata, dftstep, dftdata, dftstep);
                    }

                    // phase 2. iterate over output channels. For each output channel multiply
                    // all the input channels by the corresponding weights and sum the results.
                    // all this is done in the Fourier domain.
                    // When the sum is computed, apply the inverse DFT, then add bias and save
                    // the results.
                    for( int ocn = 0; ocn < outCn; ocn++, data_out += outPlaneSize )
                    {
                        float* odata = out_spectrum.ptr<float>();
                        memset(odata, 0, dft_elems*sizeof(odata[0]));

                        for( int cn = 0; cn < inpCn; cn++ )
                        {
                            const float* wptr = wptr0 + ((ocn + startOutCn)*inpCn + cn)*dft_elems;
                            const float* sdata = sdata0 + cn*dft_elems;

                            odata[0] += sdata[0]*wptr[0];
                            odata[dft_w-1] += sdata[dft_w-1]*wptr[dft_w-1];
                            odata[dft_elems-dft_w] += sdata[dft_elems-dft_w]*wptr[dft_elems-dft_w];
                            odata[dft_elems-1] += sdata[dft_elems-1]*wptr[dft_elems-1];

                            for( i = 1; i < dft_h-1; i += 2 )
                            {
                                int re = i*dft_w, im = re + dft_w;
                                odata[re] += sdata[re]*wptr[re] + sdata[im]*wptr[im];
                                odata[im] += sdata[im]*wptr[re] - sdata[re]*wptr[im];
                                re += dft_w-1; im += dft_w-1;
                                odata[re] += sdata[re]*wptr[re] + sdata[im]*wptr[im];
                                odata[im] += sdata[im]*wptr[re] - sdata[re]*wptr[im];
                            }

                            for( i = 0; i < dft_h; i++ )
                            {
                                for( j = 1; j < dft_w-1; j += 2 )
                                {
                                    int idx = i*dft_w + j;
                                    float re = sdata[idx], im = sdata[idx+1];
                                    float wre = wptr[idx], wim = wptr[idx+1];
                                    float ore = odata[idx], oim = odata[idx+1];
                                    odata[idx] = ore + re*wre + im*wim;
                                    odata[idx+1] = oim + im*wre - re*wim;
                                }
                            }
                        }
                        dft2d_inv->apply((const uchar*)odata, dftstep, (uchar*)odata, dftstep);
                        float bias = biasptr[ocn];
                        for( i = 0; i < bh; i++ )
                        {
                            for( j = 0; j < bw; j++ )
                            {
                                data_out[i*out_w + j] = odata[i*dft_w + j] + bias;
                            }
                        }
                    }
                }
            }
        }
    };*/

    void forward(std::vector<Mat*> &inputs, std::vector<Mat> &outputs, std::vector<Mat> &internals)
    {
        /*printf("conv %s: input (%d x %d x %d x %d), kernel (%d x %d), pad (%d x %d), stride (%d x %d), dilation (%d x %d)\n",
               name.c_str(), inputs[0]->size[0], inputs[0]->size[1], inputs[0]->size[2], inputs[0]->size[3],
               kernel.width, kernel.height, pad.width, pad.height,
               stride.width, stride.height, dilation.width, dilation.height);*/
        CV_Assert(inputs.size() == (size_t)1 && inputs[0]->size[1] % blobs[0].size[1] == 0);
        int ngroups = inputs[0]->size[1]/blobs[0].size[1];
        CV_Assert(outputs[0].size[1] % ngroups == 0);

        int outCn = blobs[0].size[0];

        if( weightsMat.empty() )
        {
            Mat wm = blobs[0].reshape(1, outCn);
            if( wm.step1() % VEC_ALIGN != 0 )
            {
                int newcols = (int)alignSize(wm.step1(), VEC_ALIGN);
                Mat wm_buffer = Mat(outCn, newcols, wm.type());
                Mat wm_padding = wm_buffer.colRange(wm.cols, newcols);
                wm_padding.setTo(Scalar::all(0.));
                Mat wm_aligned = wm_buffer.colRange(0, wm.cols);
                wm.copyTo(wm_aligned);
                wm = wm_aligned;
            }
            weightsMat = wm;
        }
        Mat biasesMat = hasBias() ? blobs[1].reshape(1, outCn) : Mat();

        int nstripes = std::max(getNumThreads(), 1);

        /*if( stride == Size(1, 1) && dilation == Size(1, 1) && kernel.width >= 3 && kernel.height >= 3 )
        {

            ParallelDFTConv::run(*inputs[0], outputs[0], weightsMat, biasesMat,
                                 kernel, pad, ngroups, nstripes, activ.get());
        }
        else*/
        {
            ParallelConv::run(*inputs[0], outputs[0], weightsMat, biasesMat,
                              kernel, pad, stride, dilation, ngroups, nstripes, activ.get());
        }
    }

    virtual int64 getFLOPS(const std::vector<MatShape> &inputs,
                           const std::vector<MatShape> &outputs) const
    {
        CV_Assert(inputs.size() == outputs.size());

        int64 flops = 0;
        for (int i = 0; i < inputs.size(); i++)
        {
            flops += total(outputs[i])*(2*kernel.area()*inputs[i][1] + 1);
        }

        return flops;
    }
};

class DeConvolutionLayerImpl : public BaseConvolutionLayerImpl
{
public:
    Mat weightsMat, biasesMat;

    MatShape computeColRowShape(const MatShape &inpShape, const MatShape &outShape) const
    {
        int inpCn = inpShape[1];
        int inpH = inpShape[2];
        int inpW = inpShape[3];
        int outCn = outShape[1];
        int ngroups = inpCn / blobs[0].size[1];
        int outGroupCn = outCn / ngroups;
        int ksize = outGroupCn * kernel.height * kernel.width;
        return shape(ksize, inpH * inpW);
    }

    bool getMemoryShapes(const std::vector<MatShape> &inputs,
                         const int requiredOutputs,
                         std::vector<MatShape> &outputs,
                         std::vector<MatShape> &internals) const
    {
        CV_Assert(!hasBias() || blobs[1].total() == (size_t)blobs[0].size[0]);
        CV_Assert(inputs.size() != 0);

        int inpCn = inputs[0][1];
        int inpH = inputs[0][2];
        int inpW = inputs[0][3];

        int outH = stride.height * (inpH - 1) + kernel.height - 2 * pad.height + adjustPad.height;
        int outW = stride.width * (inpW - 1) + kernel.width - 2 * pad.width + adjustPad.width;
        int outCn = blobs[0].size[0];

        int ngroups = inpCn / blobs[0].size[1];

        CV_Assert(inpCn % ngroups == 0 && outCn % ngroups == 0);
        CV_Assert(blobs[0].size[0] == outCn && blobs[0].size[1] == inpCn / ngroups);

        int dims[] = {inputs[0][0], outCn, outH, outW};
        outputs.resize(inputs.size(), shape(dims));

        internals.push_back(MatShape());
        if (!is1x1())
            internals[0] = computeColRowShape(inputs[0], outputs[0]);

        if (hasBias())
            internals.push_back(shape(1, outH*outW));

        return false;
    }

    class MatMulInvoker : public ParallelLoopBody
    {
    public:
        MatMulInvoker(const Mat& a, const Mat& b, Mat& c, int nstripes)
        {
            a_ = &a;
            b_ = &b;
            c_ = &c;
            nstripes_ = nstripes;
            useAVX2 = checkHardwareSupport(CPU_AVX2);
        }

        void operator()(const Range& range_) const
        {
            int stripeSize = (int)alignSize((b_->cols + nstripes_ - 1)/nstripes_, 16);
            Range range(range_.start*stripeSize, std::min(range_.end*stripeSize, b_->cols));
            int mmax = a_->rows;
            int nmax = range.end - range.start;
            int kmax = a_->cols;
            int m, n, k;
            const float* aptr = a_->ptr<float>();
            const float* bptr = b_->ptr<float>() + range.start;
            float* cptr = c_->ptr<float>() + range.start;
            size_t astep = a_->step1();
            size_t bstep = b_->step1();
            size_t cstep = c_->step1();

        #if CV_DNN_TRY_AVX2
            if( useAVX2 )
                fastGEMM_avx2( aptr, astep, bptr, bstep, cptr, cstep, mmax, kmax, nmax );
            else
        #endif
            for( m = 0; m < mmax; m += 2 )
            {
                float* dst0 = cptr + cstep*m;
                float* dst1 = cptr + cstep*std::min(m+1, mmax-1);
                const float* aptr0 = aptr + astep*m;
                const float* aptr1 = aptr + astep*std::min(m+1, mmax-1);

                for( n = 0; n < nmax; n++ )
                {
                    dst0[n] = 0.f;
                    dst1[n] = 0.f;
                }

                for( k = 0; k < kmax; k += 4 )
                {
                    float alpha00 = aptr0[k];
                    float alpha01 = aptr1[k];
                    float alpha10 = 0.f, alpha11 = 0.f;
                    float alpha20 = 0.f, alpha21 = 0.f;
                    float alpha30 = 0.f, alpha31 = 0.f;
                    const float* bptr0 = bptr + k*bstep;
                    const float* bptr1 = bptr0;
                    const float* bptr2 = bptr0;
                    const float* bptr3 = bptr0;

                    if( k+1 < kmax )
                    {
                        alpha10 = aptr0[k+1];
                        alpha11 = aptr1[k+1];
                        bptr1 = bptr0 + bstep;
                        if( k+2 < kmax )
                        {
                            alpha20 = aptr0[k+2];
                            alpha21 = aptr1[k+2];
                            bptr2 = bptr1 + bstep;
                            if( k+3 < kmax )
                            {
                                alpha30 = aptr0[k+3];
                                alpha31 = aptr1[k+3];
                                bptr3 = bptr2 + bstep;
                            }
                        }
                    }
                    n = 0;

                #if CV_SIMD128
                    v_float32x4 a00 = v_setall_f32(alpha00);
                    v_float32x4 a01 = v_setall_f32(alpha01);
                    v_float32x4 a10 = v_setall_f32(alpha10);
                    v_float32x4 a11 = v_setall_f32(alpha11);
                    v_float32x4 a20 = v_setall_f32(alpha20);
                    v_float32x4 a21 = v_setall_f32(alpha21);
                    v_float32x4 a30 = v_setall_f32(alpha30);
                    v_float32x4 a31 = v_setall_f32(alpha31);

                    for( ; n <= nmax - 4; n += 4 )
                    {
                        v_float32x4 b0 = v_load(bptr0 + n);
                        v_float32x4 b1 = v_load(bptr1 + n);
                        v_float32x4 b2 = v_load(bptr2 + n);
                        v_float32x4 b3 = v_load(bptr3 + n);
                        v_float32x4 d0 = v_load(dst0 + n);
                        v_float32x4 d1 = v_load(dst1 + n);
                        d0 += b0*a00;
                        d1 += b0*a01;
                        d0 += b1*a10;
                        d1 += b1*a11;
                        d0 += b2*a20;
                        d1 += b2*a21;
                        d0 += b3*a30;
                        d1 += b3*a31;
                        v_store(dst0 + n, d0);
                        v_store(dst1 + n, d1);
                    }
                #endif

                    for( ; n < nmax; n++ )
                    {
                        float b0 = bptr0[n], b1 = bptr1[n];
                        float b2 = bptr2[n], b3 = bptr3[n];
                        float d0 = dst0[n] + alpha00*b0 + alpha10*b1 + alpha20*b2 + alpha30*b3;
                        float d1 = dst1[n] + alpha01*b0 + alpha11*b1 + alpha21*b2 + alpha31*b3;
                        dst0[n] = d0;
                        dst1[n] = d1;
                    }
                }
            }
        }

        const Mat *a_, *b_;
        Mat* c_;
        int nstripes_;
        bool useAVX2;
    };

    class Col2ImInvoker : public cv::ParallelLoopBody
    {
    public:
        const float* data_col;
        const float* biasvec;
        int channels, height, width;
        int kernel_h, kernel_w;
        int pad_h, pad_w;
        int stride_h, stride_w;
        float* data_im;
        int height_col, width_col;
        int nstripes;
        bool is1x1;

        Col2ImInvoker() {}

        static void run(const float* data_col,
                        int channels, int height, int width,
                        int kernel_h, int kernel_w,
                        int pad_h, int pad_w,
                        int stride_h, int stride_w,
                        float* data_im,
                        const float* biasvec,
                        bool is1x1)
        {
            const int nstripes = getNumThreads();

            Col2ImInvoker t;
            t.data_col = data_col;
            t.data_im = data_im;
            t.channels = channels; t.height = height; t.width = width;
            t.kernel_h = kernel_h; t.kernel_w = kernel_w;
            t.pad_h = pad_h; t.pad_w = pad_w;
            t.stride_h = stride_h; t.stride_w = stride_w;
            t.height_col = (height + 2 * pad_h - kernel_h) / stride_h + 1;
            t.width_col = (width + 2 * pad_w - kernel_w) / stride_w + 1;
            t.nstripes = nstripes;
            t.is1x1 = is1x1;
            t.biasvec = biasvec;

            parallel_for_(Range(0, nstripes), t, nstripes);
        }

        virtual void operator ()(const Range &r) const
        {
            const float* data_col_ = data_col;
            float* data_im_ = data_im;
            int coeff_h = (1 - stride_h * kernel_w * height_col) * width_col;
            int coeff_w = (1 - stride_w * height_col * width_col);
            size_t total = (size_t)channels * height * width;
            size_t stripeSize = (total + nstripes - 1)/nstripes;
            size_t startIndex = r.start*stripeSize;
            size_t endIndex = std::min(r.end*stripeSize, total);
            int w = (int)(startIndex % width + pad_w);
            int h = (int)((startIndex / width) % height + pad_h);
            int c = (int)(startIndex / (width * height));
            int h_col_start = (h < kernel_h) ? 0 : (h - kernel_h) / stride_h + 1;
            int h_col_end = std::min(h / stride_h + 1, height_col);
            int plane_size_col = height_col * width_col;
            int offset = (c * kernel_h * kernel_w + h * kernel_w + w) * plane_size_col;
            bool is1x1_ = is1x1;
            const float* biasvec_ = biasvec;

            for (size_t index = startIndex; index < endIndex; index++)
            {
                // compute the start and end of the output
                int w_col_start = (w < kernel_w) ? 0 : (w - kernel_w) / stride_w + 1;
                int w_col_end = std::min(w / stride_w + 1, width_col);
                float val;

                if( is1x1_ )
                    val = data_im_[index];
                else
                {
                    val = 0.f;
                    for (int h_col = h_col_start; h_col < h_col_end; ++h_col) {
                        for (int w_col = w_col_start; w_col < w_col_end; ++w_col) {
                            val += data_col_[offset + h_col * coeff_h + w_col * coeff_w];
                        }
                    }
                }
                data_im_[index] = val + biasvec_[c];

                offset += plane_size_col;
                if( ++w >= width + pad_w )
                {
                    w = (int)((index + 1)% width + pad_w);
                    h = (int)(((index + 1) / width) % height + pad_h);
                    c = (int)((index + 1) / (width * height));
                    h_col_start = (h < kernel_h) ? 0 : (h - kernel_h) / stride_h + 1;
                    h_col_end = std::min(h / stride_h + 1, height_col);
                    offset = (c * kernel_h * kernel_w + h * kernel_w + w) * plane_size_col;
                }
            }
        }
    };

    void forward(std::vector<Mat *> &inputs, std::vector<Mat> &outputs, std::vector<Mat> &internals)
    {
        if (hasBias())
            internals[1].setTo(1);

        int outCn = blobs[0].size[0];
        int inpCn = inputs[0]->size[1];
        bool is1x1flag = is1x1();
        int nstripes = getNumThreads();

        if( weightsMat.empty() )
        {
            transpose(blobs[0].reshape(1, inpCn), weightsMat);
            biasesMat = hasBias() ? blobs[1].reshape(1, outCn) : Mat::zeros(outCn, 1, CV_32F);
        }

        for (size_t ii = 0; ii < outputs.size(); ii++)
        {
            int ngroups = inpCn / blobs[0].size[1];
            int inpGroupCn = blobs[0].size[1];
            int outGroupCn = outCn / ngroups;
            const Mat& inp = *inputs[ii];
            Mat& out = outputs[ii];
            int numImg = inp.size[0];
            int outH = out.size[2], outW = out.size[3];

            Mat convBlob = inputs[ii]->reshape(1, numImg*inpCn);
            Mat decnBlob = out.reshape(1, numImg*outCn);

            for (int n = 0; n < numImg; n++)
            {
                for (int g = 0; g < ngroups; g++)
                {
                    Mat dstMat = decnBlob.rowRange(_Range((g + n * ngroups) * outGroupCn, outGroupCn));
                    Mat &colMat = is1x1flag ? dstMat : internals[0];

                    Mat convMat = convBlob.rowRange(_Range((g + n * ngroups) * inpGroupCn, inpGroupCn));
                    Mat wghtMat = weightsMat.colRange(_Range(g * inpGroupCn, inpGroupCn));
                    Mat curBiasMat = biasesMat.rowRange(_Range(g * outGroupCn, outGroupCn));

                    //gemm(wghtMat, convMat, 1, colMat, 0, colMat, 0);
                    MatMulInvoker mminvoker(wghtMat, convMat, colMat, nstripes);
                    parallel_for_(Range(0, nstripes), mminvoker, nstripes);

                    Col2ImInvoker::run(colMat.ptr<float>(), outGroupCn, outH, outW,
                                       kernel.height, kernel.width, pad.height, pad.width,
                                       stride.height, stride.width, dstMat.ptr<float>(),
                                       curBiasMat.ptr<float>(), is1x1flag);
                }
            }
        }
    }

    virtual Ptr<BackendNode> initHalide(const std::vector<Ptr<BackendWrapper> > &inputs)
    {
#ifdef HAVE_HALIDE
        Halide::Buffer<float> inputBuffer = halideBuffer(inputs[0]);

        int inW, inH, inC, inN, outC = blobs[0].size[0];
        getCanonicalSize(inputBuffer, &inW, &inH, &inC, &inN);

        if (inC / blobs[0].size[1] != 1)
            CV_Error(cv::Error::StsNotImplemented,
                     "Halide backend for Deconvolution with group > 1 is not implemented");

        Halide::Var x("x"), y("y"), c("c"), n("n");
        Halide::Func top = (name.empty() ? Halide::Func() : Halide::Func(name));
        Halide::Func padded_input(name + "_constant_exterior");
        auto weights = wrapToHalideBuffer(blobs[0], {kernel.width,
                                                     kernel.height, outC, inC});

        Halide::Func dilated_input("dilated_input");
        dilated_input(x, y, c, n) = 0.0f;
        Halide::RDom r1(0, inW, 0, inH);
        dilated_input(r1.x * stride.width, r1.y * stride.height, c, n) =
              inputBuffer(r1.x, r1.y, c, n);
        dilated_input.compute_root();

        Halide::Func bounded =
            Halide::BoundaryConditions::constant_exterior(dilated_input, 0,
                                                          0, (inW - 1) * stride.width + 1,
                                                          0, (inH - 1) * stride.height + 1,
                                                          0, inC, 0, inN);
        padded_input(x, y, c, n) = bounded(x, y, c, n);

        Halide::RDom r(0, kernel.width, 0, kernel.height, 0, inC);
        Halide::Expr topExpr = sum(
            padded_input(x + pad.width - r.x, y + pad.height - r.y, r.z, n) *
            weights(r.x, r.y, c, r.z));
        if (hasBias())
        {
            auto bias = wrapToHalideBuffer(blobs[1], {outC});
            topExpr += bias(c);
        }
        top(x, y, c, n) = topExpr;
        return Ptr<BackendNode>(new HalideBackendNode({ padded_input, top }));
#endif  // HAVE_HALIDE
        return Ptr<BackendNode>();
    }

    virtual int64 getFLOPS(const std::vector<MatShape> &inputs,
                           const std::vector<MatShape> &outputs) const
    {
        CV_Assert(inputs.size() == outputs.size());

        float flops = 0;
        int outChannels = blobs[0].size[0];

        for (int i = 0; i < inputs.size(); i++)
        {
            flops += 2*outChannels*kernel.area()*total(inputs[i]);
        }

        return flops;
    }
};

//Convolution and Deconvolution
static void initConvDeconvLayerFromCaffe(Ptr<BaseConvolutionLayer> l, const LayerParams &params)
{
    l->setParamsFrom(params);
    getConvolutionKernelParams(params, l->kernel.height, l->kernel.width, l->pad.height,
                               l->pad.width, l->stride.height, l->stride.width, l->dilation.height,
                               l->dilation.width, l->padMode);

    bool bias = params.get<bool>("bias_term", true);
    int numOutput = params.get<int>("num_output");
    int ngroups = params.get<int>("group", 1);

    l->adjustPad.height = params.get<int>("adj_h", 0);
    l->adjustPad.width = params.get<int>("adj_w", 0);

    CV_Assert(numOutput % ngroups == 0);
    CV_Assert((bias && l->blobs.size() == 2) || (!bias && l->blobs.size() == 1));
    CV_Assert(l->adjustPad.width < l->stride.width &&
              l->adjustPad.height < l->stride.height);
}

Ptr<BaseConvolutionLayer> ConvolutionLayer::create(const LayerParams &params)
{
    Ptr<BaseConvolutionLayer> l(new ConvolutionLayerImpl);
    initConvDeconvLayerFromCaffe(l, params);
    return l;
}

Ptr<BaseConvolutionLayer> DeconvolutionLayer::create(const LayerParams &params)
{
    Ptr<BaseConvolutionLayer> l(new DeConvolutionLayerImpl);
    initConvDeconvLayerFromCaffe(l, params);

    return l;
}

}
}
