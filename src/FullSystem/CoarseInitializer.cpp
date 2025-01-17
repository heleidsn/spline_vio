/**
 * This file is part of DSO.
 *
 * Copyright 2016 Technical University of Munich and Intel.
 * Developed by Jakob Engel <engelj at in dot tum dot de>,
 * for more information see <http://vision.in.tum.de/dso>.
 * If you use this code, please cite the respective publications as
 * listed on the above website.
 *
 * DSO is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DSO is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DSO. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * KFBuffer.cpp
 *
 *  Created on: Jan 7, 2014
 *      Author: engelj
 */

#include "CoarseInitializer.h"
#include "FullSystem.h"
#include "HessianBlocks.h"
#include "PixelSelector.h"
#include "PixelSelector2.h"
#include "Residuals.h"
#include "util/nanoflann.h"

#if !defined(__SSE3__) && !defined(__SSE2__) && !defined(__SSE1__)
#include "SSE2NEON.h"
#endif

namespace dso {

CoarseInitializer::CoarseInitializer(int ww, int hh)
    : thisToNext_aff(0, 0), thisToNext(SE3()) {
  for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
    points[lvl] = 0;
    numPoints[lvl] = 0;
  }

  JbBuffer = new Vec10f[ww * hh];
  JbBuffer_new = new Vec10f[ww * hh];

  frameID = -1;
  fixAffine = true;
  printDebug = false;

  wM.diagonal()[0] = wM.diagonal()[1] = wM.diagonal()[2] = SCALE_XI_ROT;
  wM.diagonal()[3] = wM.diagonal()[4] = wM.diagonal()[5] = SCALE_XI_TRANS;
  wM.diagonal()[6] = SCALE_A;
  wM.diagonal()[7] = SCALE_B;
}

CoarseInitializer::~CoarseInitializer() {
  for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
    if (points[lvl] != 0)
      delete[] points[lvl];
  }

  delete[] JbBuffer;
  delete[] JbBuffer_new;
  for (float *ptr : ptr_to_delete_)
    delete[] ptr;
  ptr_to_delete_.clear();
}

void CoarseInitializer::makeK(CalibHessian *HCalib) {
  w_[0] = wG[0];
  h_[0] = hG[0];

  fx_[0] = HCalib->fxl();
  fy_[0] = HCalib->fyl();
  cx_[0] = HCalib->cxl();
  cy_[0] = HCalib->cyl();

  for (int level = 1; level < pyrLevelsUsed; ++level) {
    w_[level] = w_[0] >> level;
    h_[level] = h_[0] >> level;
    fx_[level] = fx_[level - 1] * 0.5;
    fy_[level] = fy_[level - 1] * 0.5;
    cx_[level] = (cx_[0] + 0.5) / ((int)1 << level) - 0.5;
    cy_[level] = (cy_[0] + 0.5) / ((int)1 << level) - 0.5;
  }

  for (int level = 0; level < pyrLevelsUsed; ++level) {
    Mat33f K;
    K << fx_[level], 0.0, cx_[level], 0.0, fy_[level], cy_[level], 0.0, 0.0,
        1.0;
    Ki_[level] = K.inverse();
  }
}

void CoarseInitializer::makeCoarseDepth() {
  // make coarse tracking templates for latstRef.
  memset(idepth_[0], 0, sizeof(float) * w_[0] * h_[0]);
  memset(weight_sums_[0], 0, sizeof(float) * w_[0] * h_[0]);

  for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
    int npts = numPoints[lvl];
    Pnt *ptsl = points[lvl];
    for (int i = 0; i < npts; i++) {
      if (ptsl[i].isGood) {
        int u = ptsl[i].u + 0.5f;
        int v = ptsl[i].v + 0.5f;
        float id = ptsl[i].iR;
        float weight = 1.0;
        idepth_[lvl][u + w_[lvl] * v] += id * weight;
        weight_sums_[lvl][u + w_[lvl] * v] += weight;
      }
    }
  }

  // dilate idepth_ by 1.
  for (int lvl = 0; lvl < 2; lvl++) {
    int numIts = 1;

    for (int it = 0; it < numIts; it++) {
      int wh = w_[lvl] * h_[lvl] - w_[lvl];
      int wl = w_[lvl];
      float *weightSumsl = weight_sums_[lvl];
      float *weightSumsl_bak = weight_sums_bak_[lvl];
      memcpy(weightSumsl_bak, weightSumsl, w_[lvl] * h_[lvl] * sizeof(float));
      // dotnt need to make a temp copy of depth, since I only
      // read values with weightSumsl>0, and write ones with
      // weightSumsl<=0.
      float *idepthl = idepth_[lvl];
      for (int i = w_[lvl]; i < wh; i++) {
        if (weightSumsl_bak[i] <= 0) {
          float sum = 0, num = 0, numn = 0;
          if (weightSumsl_bak[i + 1 + wl] > 0) {
            sum += idepthl[i + 1 + wl];
            num += weightSumsl_bak[i + 1 + wl];
            numn++;
          }
          if (weightSumsl_bak[i - 1 - wl] > 0) {
            sum += idepthl[i - 1 - wl];
            num += weightSumsl_bak[i - 1 - wl];
            numn++;
          }
          if (weightSumsl_bak[i + wl - 1] > 0) {
            sum += idepthl[i + wl - 1];
            num += weightSumsl_bak[i + wl - 1];
            numn++;
          }
          if (weightSumsl_bak[i - wl + 1] > 0) {
            sum += idepthl[i - wl + 1];
            num += weightSumsl_bak[i - wl + 1];
            numn++;
          }
          if (numn > 0) {
            idepthl[i] = sum / numn;
            weightSumsl[i] = num / numn;
          }
        }
      }
    }
  }

  // dilate idepth_ by 1 (2 on lower levels).
  for (int lvl = 2; lvl < pyrLevelsUsed; lvl++) {
    int wh = w_[lvl] * h_[lvl] - w_[lvl];
    int wl = w_[lvl];
    float *weightSumsl = weight_sums_[lvl];
    float *weightSumsl_bak = weight_sums_bak_[lvl];
    memcpy(weightSumsl_bak, weightSumsl, w_[lvl] * h_[lvl] * sizeof(float));
    float *idepthl =
        idepth_[lvl]; // dotnt need to make a temp copy of depth,
                      // since I only read values with weightSumsl>0,
                      // and write ones with weightSumsl<=0.
    for (int i = w_[lvl]; i < wh; i++) {
      if (weightSumsl_bak[i] <= 0) {
        float sum = 0, num = 0, numn = 0;
        if (weightSumsl_bak[i + 1] > 0) {
          sum += idepthl[i + 1];
          num += weightSumsl_bak[i + 1];
          numn++;
        }
        if (weightSumsl_bak[i - 1] > 0) {
          sum += idepthl[i - 1];
          num += weightSumsl_bak[i - 1];
          numn++;
        }
        if (weightSumsl_bak[i + wl] > 0) {
          sum += idepthl[i + wl];
          num += weightSumsl_bak[i + wl];
          numn++;
        }
        if (weightSumsl_bak[i - wl] > 0) {
          sum += idepthl[i - wl];
          num += weightSumsl_bak[i - wl];
          numn++;
        }
        if (numn > 0) {
          idepthl[i] = sum / numn;
          weightSumsl[i] = num / numn;
        }
      }
    }
  }

  // normalize idepths and weights.
  for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
    float *weightSumsl = weight_sums_[lvl];
    float *idepthl = idepth_[lvl];
    Eigen::Vector3f *dIRefl = firstFrame->dIp[lvl];

    int wl = w_[lvl], hl = h_[lvl];

    int lpc_n = 0;
    float *lpc_u = pc_u_[lvl];
    float *lpc_v = pc_v_[lvl];
    float *lpc_idepth = pc_idepth_[lvl];
    float *lpc_color = pc_color_[lvl];

    for (int y = 2; y < hl - 2; y++)
      for (int x = 2; x < wl - 2; x++) {
        int i = x + y * wl;

        if (weightSumsl[i] > 0) {
          idepthl[i] /= weightSumsl[i];
          lpc_u[lpc_n] = x;
          lpc_v[lpc_n] = y;
          lpc_idepth[lpc_n] = idepthl[i];
          lpc_color[lpc_n] = dIRefl[i][0];

          if (!std::isfinite(lpc_color[lpc_n]) || !(idepthl[i] > 0)) {
            idepthl[i] = -1;
            continue; // just skip if something is wrong.
          }
          lpc_n++;
        } else
          idepthl[i] = -1;

        weightSumsl[i] = 1;
      }

    pc_n_[lvl] = lpc_n;
  }
}

bool CoarseInitializer::trackFrame(
    FrameHessian *newFrameHessian,
    std::vector<IOWrap::Output3DWrapper *> &wraps) {
  newFrame = newFrameHessian;

  for (IOWrap::Output3DWrapper *ow : wraps)
    ow->pushLiveFrame(newFrameHessian);

  int maxIterations[] = {5, 5, 10, 30, 50};

  alphaK = 2.5 * 2.5; //*freeDebugParam1*freeDebugParam1;
  alphaW = 150 * 150; //*freeDebugParam2*freeDebugParam2;
  regWeight = 0.8;    //*freeDebugParam4;
  couplingWeight = 1; //*freeDebugParam5;

  if (!snapped) {
    thisToNext.translation().setZero();
    for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
      int npts = numPoints[lvl];
      Pnt *ptsl = points[lvl];
      for (int i = 0; i < npts; i++) {
        ptsl[i].iR = 1;
        ptsl[i].idepth_new = 1;  // 将所有points上的逆深度都设为1
        ptsl[i].lastHessian = 0;
      }
    }
  }

  SE3 refToNew_current = thisToNext;
  AffLight refToNew_aff_current = thisToNext_aff;

  if (firstFrame->ab_exposure > 0 && newFrame->ab_exposure > 0)
    refToNew_aff_current =
        AffLight(logf(newFrame->ab_exposure / firstFrame->ab_exposure),
                 0); // coarse approximation.

  Vec3f latestRes = Vec3f::Zero();
  for (int lvl = pyrLevelsUsed - 1; lvl >= 0; lvl--) {

    if (lvl < pyrLevelsUsed - 1)
      propagateDown(lvl + 1);

    Mat88f H, Hsc;
    Vec8f b, bsc;
    resetPoints(lvl);
    Vec3f resOld = calcResAndGS(lvl, H, b, Hsc, bsc, refToNew_current,
                                refToNew_aff_current, false);  // 计算Hessian矩阵等信息
    applyStep(lvl);

    float lambda = 0.1;
    float eps = 1e-4;
    int fails = 0;

    if (printDebug) {
      printf("lvl %d, it %d (l=%f) %s: %.3f+%.5f -> %.3f+%.5f (%.3f->%.3f) "
             "(|inc| = %f)! \t",
             lvl, 0, lambda, "INITIA", sqrtf((float)(resOld[0] / resOld[2])),
             sqrtf((float)(resOld[1] / resOld[2])),
             sqrtf((float)(resOld[0] / resOld[2])),
             sqrtf((float)(resOld[1] / resOld[2])),
             (resOld[0] + resOld[1]) / resOld[2],
             (resOld[0] + resOld[1]) / resOld[2], 0.0f);
      std::cout << refToNew_current.log().transpose() << " AFF "
                << refToNew_aff_current.vec().transpose() << "\n";
    }

    int iteration = 0;
    while (true) {
      Mat88f Hl = H;
      for (int i = 0; i < 8; i++)
        Hl(i, i) *= (1 + lambda);
      Hl -= Hsc * (1 / (1 + lambda));
      Vec8f bl = b - bsc * (1 / (1 + lambda));

      Hl = wM * Hl * wM * (0.01f / (w_[lvl] * h_[lvl]));
      bl = wM * bl * (0.01f / (w_[lvl] * h_[lvl]));

      Vec8f inc;
      if (fixAffine) {
        inc.head<6>() =
            -(wM.toDenseMatrix().topLeftCorner<6, 6>() *
              (Hl.topLeftCorner<6, 6>().ldlt().solve(bl.head<6>())));
        inc.tail<2>().setZero();
      } else
        inc = -(wM * (Hl.ldlt().solve(bl))); //=-H^-1 * b.

      SE3 refToNew_new =
          SE3::exp(inc.head<6>().cast<double>()) * refToNew_current;
      AffLight refToNew_aff_new = refToNew_aff_current;
      refToNew_aff_new.a += inc[6];
      refToNew_aff_new.b += inc[7];
      doStep(lvl, lambda, inc);

      Mat88f H_new, Hsc_new;
      Vec8f b_new, bsc_new;
      Vec3f resNew = calcResAndGS(lvl, H_new, b_new, Hsc_new, bsc_new,
                                  refToNew_new, refToNew_aff_new, false);
      Vec3f regEnergy = calcEC(lvl);

      float eTotalNew = (resNew[0] + resNew[1] + regEnergy[1]);
      float eTotalOld = (resOld[0] + resOld[1] + regEnergy[0]);

      bool accept = eTotalOld > eTotalNew;

      if (printDebug) {
        printf("lvl %d, it %d (l=%f) %s: %.5f + %.5f + %.5f -> %.5f + %.5f + "
               "%.5f (%.2f->%.2f) (|inc| = %f)! \t",
               lvl, iteration, lambda, (accept ? "ACCEPT" : "REJECT"),
               sqrtf((float)(resOld[0] / resOld[2])),
               sqrtf((float)(regEnergy[0] / regEnergy[2])),
               sqrtf((float)(resOld[1] / resOld[2])),
               sqrtf((float)(resNew[0] / resNew[2])),
               sqrtf((float)(regEnergy[1] / regEnergy[2])),
               sqrtf((float)(resNew[1] / resNew[2])), eTotalOld / resNew[2],
               eTotalNew / resNew[2], inc.norm());
        std::cout << refToNew_new.log().transpose() << " AFF "
                  << refToNew_aff_new.vec().transpose() << "\n";
      }

      if (accept) {

        if (resNew[1] == alphaK * numPoints[lvl])
          snapped = true;
        H = H_new;
        b = b_new;
        Hsc = Hsc_new;
        bsc = bsc_new;
        resOld = resNew;
        refToNew_aff_current = refToNew_aff_new;
        refToNew_current = refToNew_new;
        applyStep(lvl);
        optReg(lvl);
        lambda *= 0.5;
        fails = 0;
        if (lambda < 0.0001)
          lambda = 0.0001;
      } else {
        fails++;
        lambda *= 4;
        if (lambda > 10000)
          lambda = 10000;
      }

      bool quitOpt = false;

      if (!(inc.norm() > eps) || iteration >= maxIterations[lvl] ||
          fails >= 2) {
        Mat88f H, Hsc;
        Vec8f b, bsc;

        quitOpt = true;
      }

      if (quitOpt)
        break;
      iteration++;
    }
    latestRes = resOld;
  }

  thisToNext = refToNew_current;
  thisToNext_aff = refToNew_aff_current;

  for (int i = 0; i < pyrLevelsUsed - 1; i++)
    propagateUp(i);

  frameID++;
  if (!snapped)
    snappedAt = 0;

  if (snapped && snappedAt == 0)
    snappedAt = frameID;

  debugPlot(0, wraps);

  return snapped && frameID > snappedAt + 5;
}

void CoarseInitializer::debugPlot(
    int lvl, std::vector<IOWrap::Output3DWrapper *> &wraps) {
  bool needCall = false;
  for (IOWrap::Output3DWrapper *ow : wraps)
    needCall = needCall || ow->needPushDepthImage();
  if (!needCall)
    return;

  int wl = w_[lvl], hl = h_[lvl];
  Eigen::Vector3f *colorRef = firstFrame->dIp[lvl];

  MinimalImageB3 iRImg(wl, hl);

  for (int i = 0; i < wl * hl; i++)
    iRImg.at(i) = Vec3b(colorRef[i][0], colorRef[i][0], colorRef[i][0]);

  int npts = numPoints[lvl];

  float nid = 0, sid = 0;
  for (int i = 0; i < npts; i++) {
    Pnt *point = points[lvl] + i;
    if (point->isGood) {
      nid++;
      sid += point->iR;
    }
  }
  float fac = nid / sid;

  for (int i = 0; i < npts; i++) {
    Pnt *point = points[lvl] + i;

    if (!point->isGood)
      iRImg.setPixel9(point->u + 0.5f, point->v + 0.5f, Vec3b(0, 0, 0));

    else
      iRImg.setPixel9(point->u + 0.5f, point->v + 0.5f,
                      makeRainbow3B(point->iR * fac));
  }

  // IOWrap::displayImage("idepth-R", &iRImg, false);
  for (IOWrap::Output3DWrapper *ow : wraps)
    ow->pushDepthImage(&iRImg);
}

// calculates residual, Hessian and Hessian-block neede for re-substituting
// depth.
Vec3f CoarseInitializer::calcResAndGS(int lvl, Mat88f &H_out, Vec8f &b_out,
                                      Mat88f &H_out_sc, Vec8f &b_out_sc,
                                      const SE3 &refToNew,
                                      AffLight refToNew_aff, bool plot) {
  int wl = w_[lvl], hl = h_[lvl];
  Eigen::Vector3f *colorRef = firstFrame->dIp[lvl];
  Eigen::Vector3f *colorNew = newFrame->dIp[lvl];

  Mat33f RKi = refToNew.rotationMatrix().cast<float>() * Ki_[lvl];
  Vec3f t = refToNew.translation().cast<float>();
  Eigen::Vector2f r2new_aff =
      Eigen::Vector2f(exp(refToNew_aff.a), refToNew_aff.b);

  float fxl = fx_[lvl];
  float fyl = fy_[lvl];
  float cxl = cx_[lvl];
  float cyl = cy_[lvl];

  Accumulator11 E;
  acc9.initialize();
  E.initialize();

  int npts = numPoints[lvl];
  Pnt *ptsl = points[lvl];
  for (int i = 0; i < npts; i++) {

    Pnt *point = ptsl + i;

    point->maxstep = 1e10;
    if (!point->isGood) {
      E.updateSingle((float)(point->energy[0]));
      point->energy_new = point->energy;
      point->isGood_new = false;
      continue;
    }

    VecNRf dp0;
    VecNRf dp1;
    VecNRf dp2;
    VecNRf dp3;
    VecNRf dp4;
    VecNRf dp5;
    VecNRf dp6;
    VecNRf dp7;
    VecNRf dd;
    VecNRf r;
    JbBuffer_new[i].setZero();

    // sum over all residuals.
    bool isGood = true;
    float energy = 0;
    for (int idx = 0; idx < patternNum; idx++) {
      int dx = patternP[idx][0];
      int dy = patternP[idx][1];

      Vec3f pt =
          RKi * Vec3f(point->u + dx, point->v + dy, 1) + t * point->idepth_new;
      float u = pt[0] / pt[2];
      float v = pt[1] / pt[2];
      float Ku = fxl * u + cxl;
      float Kv = fyl * v + cyl;
      float new_idepth = point->idepth_new / pt[2];

      if (!(Ku > 1 && Kv > 1 && Ku < wl - 2 && Kv < hl - 2 && new_idepth > 0)) {
        isGood = false;
        break;
      }

      Vec3f hitColor = getInterpolatedElement33(colorNew, Ku, Kv, wl);
      // Vec3f hitColor = getInterpolatedElement33BiCub(colorNew, Ku, Kv, wl);

      // float rlR = colorRef[point->u+dx + (point->v+dy) * wl][0];
      float rlR =
          getInterpolatedElement31(colorRef, point->u + dx, point->v + dy, wl);

      if (!std::isfinite(rlR) || !std::isfinite((float)hitColor[0])) {
        isGood = false;
        break;
      }

      float residual = hitColor[0] - r2new_aff[0] * rlR - r2new_aff[1];
      float hw = fabs(residual) < setting_huberTH
                     ? 1
                     : setting_huberTH / fabs(residual);
      energy += hw * residual * residual * (2 - hw);

      float dxdd = (t[0] - t[2] * u) / pt[2];
      float dydd = (t[1] - t[2] * v) / pt[2];

      if (hw < 1)
        hw = sqrtf(hw);
      float dxInterp = hw * hitColor[1] * fxl;
      float dyInterp = hw * hitColor[2] * fyl;
      dp0[idx] = new_idepth * dxInterp;
      dp1[idx] = new_idepth * dyInterp;
      dp2[idx] = -new_idepth * (u * dxInterp + v * dyInterp);
      dp3[idx] = -u * v * dxInterp - (1 + v * v) * dyInterp;
      dp4[idx] = (1 + u * u) * dxInterp + u * v * dyInterp;
      dp5[idx] = -v * dxInterp + u * dyInterp;
      dp6[idx] = -hw * r2new_aff[0] * rlR;
      dp7[idx] = -hw * 1;
      dd[idx] = dxInterp * dxdd + dyInterp * dydd;
      r[idx] = hw * residual;

      float maxstep = 1.0f / Vec2f(dxdd * fxl, dydd * fyl).norm();
      if (maxstep < point->maxstep)
        point->maxstep = maxstep;

      // immediately compute dp*dd' and dd*dd' in JbBuffer1.
      JbBuffer_new[i][0] += dp0[idx] * dd[idx];
      JbBuffer_new[i][1] += dp1[idx] * dd[idx];
      JbBuffer_new[i][2] += dp2[idx] * dd[idx];
      JbBuffer_new[i][3] += dp3[idx] * dd[idx];
      JbBuffer_new[i][4] += dp4[idx] * dd[idx];
      JbBuffer_new[i][5] += dp5[idx] * dd[idx];
      JbBuffer_new[i][6] += dp6[idx] * dd[idx];
      JbBuffer_new[i][7] += dp7[idx] * dd[idx];
      JbBuffer_new[i][8] += r[idx] * dd[idx];
      JbBuffer_new[i][9] += dd[idx] * dd[idx];
    }

    if (!isGood || energy > point->outlierTH * 20) {
      E.updateSingle((float)(point->energy[0]));
      point->isGood_new = false;
      point->energy_new = point->energy;
      continue;
    }

    // add into energy.
    E.updateSingle(energy);
    point->isGood_new = true;
    point->energy_new[0] = energy;

    // update Hessian matrix.
    for (int i = 0; i + 3 < patternNum; i += 4)
      acc9.updateSSE(_mm_load_ps(((float *)(&dp0)) + i),
                     _mm_load_ps(((float *)(&dp1)) + i),
                     _mm_load_ps(((float *)(&dp2)) + i),
                     _mm_load_ps(((float *)(&dp3)) + i),
                     _mm_load_ps(((float *)(&dp4)) + i),
                     _mm_load_ps(((float *)(&dp5)) + i),
                     _mm_load_ps(((float *)(&dp6)) + i),
                     _mm_load_ps(((float *)(&dp7)) + i),
                     _mm_load_ps(((float *)(&r)) + i));

    for (int i = ((patternNum >> 2) << 2); i < patternNum; i++)
      acc9.updateSingle((float)dp0[i], (float)dp1[i], (float)dp2[i],
                        (float)dp3[i], (float)dp4[i], (float)dp5[i],
                        (float)dp6[i], (float)dp7[i], (float)r[i]);
  }

  E.finish();
  acc9.finish();

  // calculate alpha energy, and decide if we cap it.
  Accumulator11 EAlpha;
  EAlpha.initialize();
  for (int i = 0; i < npts; i++) {
    Pnt *point = ptsl + i;
    if (!point->isGood_new) {
      E.updateSingle((float)(point->energy[1]));
    } else {
      point->energy_new[1] = (point->idepth_new - 1) * (point->idepth_new - 1);
      E.updateSingle((float)(point->energy_new[1]));
    }
  }
  EAlpha.finish();
  float alphaEnergy =
      alphaW * (EAlpha.A + refToNew.translation().squaredNorm() * npts);

  // printf("AE = %f * %f + %f\n", alphaW, EAlpha.A,
  // refToNew.translation().squaredNorm() * npts);

  // compute alpha opt.
  float alphaOpt;
  if (alphaEnergy > alphaK * npts) {
    alphaOpt = 0;
    alphaEnergy = alphaK * npts;
  } else {
    alphaOpt = alphaW;
  }

  acc9SC.initialize();
  for (int i = 0; i < npts; i++) {
    Pnt *point = ptsl + i;
    if (!point->isGood_new)
      continue;

    point->lastHessian_new = JbBuffer_new[i][9];

    JbBuffer_new[i][8] += alphaOpt * (point->idepth_new - 1);
    JbBuffer_new[i][9] += alphaOpt;

    if (alphaOpt == 0) {
      JbBuffer_new[i][8] += couplingWeight * (point->idepth_new - point->iR);
      JbBuffer_new[i][9] += couplingWeight;
    }

    JbBuffer_new[i][9] = 1 / (1 + JbBuffer_new[i][9]);
    acc9SC.updateSingleWeighted(
        (float)JbBuffer_new[i][0], (float)JbBuffer_new[i][1],
        (float)JbBuffer_new[i][2], (float)JbBuffer_new[i][3],
        (float)JbBuffer_new[i][4], (float)JbBuffer_new[i][5],
        (float)JbBuffer_new[i][6], (float)JbBuffer_new[i][7],
        (float)JbBuffer_new[i][8], (float)JbBuffer_new[i][9]);
  }
  acc9SC.finish();

  // printf("nelements in H: %d, in E: %d, in Hsc: %d / 9!\n", (int)acc9.num,
  // (int)E.num, (int)acc9SC.num*9);
  H_out = acc9.H.topLeftCorner<8, 8>();       // / acc9.num;
  b_out = acc9.H.topRightCorner<8, 1>();      // / acc9.num;
  H_out_sc = acc9SC.H.topLeftCorner<8, 8>();  // / acc9.num;
  b_out_sc = acc9SC.H.topRightCorner<8, 1>(); // / acc9.num;

  H_out(0, 0) += alphaOpt * npts;
  H_out(1, 1) += alphaOpt * npts;
  H_out(2, 2) += alphaOpt * npts;

  Vec3f tlog = refToNew.log().head<3>().cast<float>();
  b_out[0] += tlog[0] * alphaOpt * npts;
  b_out[1] += tlog[1] * alphaOpt * npts;
  b_out[2] += tlog[2] * alphaOpt * npts;

  return Vec3f(E.A, alphaEnergy, E.num);
}

float CoarseInitializer::rescale() {
  float factor = 20 * thisToNext.translation().norm();
  //	float factori = 1.0f/factor;
  //	float factori2 = factori*factori;
  //
  //	for(int lvl=0;lvl<pyrLevelsUsed;lvl++)
  //	{
  //		int npts = numPoints[lvl];
  //		Pnt* ptsl = points[lvl];
  //		for(int i=0;i<npts;i++)
  //		{
  //			ptsl[i].iR *= factor;
  //			ptsl[i].idepth_new *= factor;
  //			ptsl[i].lastHessian *= factori2;
  //		}
  //	}
  //	thisToNext.translation() *= factori;

  return factor;
}

Vec3f CoarseInitializer::calcEC(int lvl) {
  if (!snapped)
    return Vec3f(0, 0, numPoints[lvl]);
  AccumulatorX<2> E;
  E.initialize();
  int npts = numPoints[lvl];
  for (int i = 0; i < npts; i++) {
    Pnt *point = points[lvl] + i;
    if (!point->isGood_new)
      continue;
    float rOld = (point->idepth - point->iR);
    float rNew = (point->idepth_new - point->iR);
    E.updateNoWeight(Vec2f(rOld * rOld, rNew * rNew));

    // printf("%f %f %f!\n", point->idepth, point->idepth_new, point->iR);
  }
  E.finish();

  // printf("ER: %f %f %f!\n", couplingWeight*E.A1m[0], couplingWeight*E.A1m[1],
  // (float)E.num.numIn1m);
  return Vec3f(couplingWeight * E.A1m[0], couplingWeight * E.A1m[1], E.num);
}
void CoarseInitializer::optReg(int lvl) {
  int npts = numPoints[lvl];
  Pnt *ptsl = points[lvl];
  if (!snapped) {
    for (int i = 0; i < npts; i++)
      ptsl[i].iR = 1;
    return;
  }

  for (int i = 0; i < npts; i++) {
    Pnt *point = ptsl + i;
    if (!point->isGood)
      continue;

    float idnn[10];
    int nnn = 0;
    for (int j = 0; j < 10; j++) {
      if (point->neighbours[j] == -1)
        continue;
      Pnt *other = ptsl + point->neighbours[j];
      if (!other->isGood)
        continue;
      idnn[nnn] = other->iR;
      nnn++;
    }

    if (nnn > 2) {
      std::nth_element(idnn, idnn + nnn / 2, idnn + nnn);
      point->iR = (1 - regWeight) * point->idepth + regWeight * idnn[nnn / 2];
    }
  }
}

void CoarseInitializer::propagateUp(int srcLvl) {
  assert(srcLvl + 1 < pyrLevelsUsed);
  // set idepth of target

  int nptss = numPoints[srcLvl];
  int nptst = numPoints[srcLvl + 1];
  Pnt *ptss = points[srcLvl];
  Pnt *ptst = points[srcLvl + 1];

  // set to zero.
  for (int i = 0; i < nptst; i++) {
    Pnt *parent = ptst + i;
    parent->iR = 0;
    parent->iRSumNum = 0;
  }

  for (int i = 0; i < nptss; i++) {
    Pnt *point = ptss + i;
    if (!point->isGood)
      continue;

    Pnt *parent = ptst + point->parent;
    parent->iR += point->iR * point->lastHessian;
    parent->iRSumNum += point->lastHessian;
  }

  for (int i = 0; i < nptst; i++) {
    Pnt *parent = ptst + i;
    if (parent->iRSumNum > 0) {
      parent->idepth = parent->iR = (parent->iR / parent->iRSumNum);
      parent->isGood = true;
    }
  }

  optReg(srcLvl + 1);
}

void CoarseInitializer::propagateDown(int srcLvl) {
  assert(srcLvl > 0);
  // set idepth of target

  int nptst = numPoints[srcLvl - 1];
  Pnt *ptss = points[srcLvl];
  Pnt *ptst = points[srcLvl - 1];

  for (int i = 0; i < nptst; i++) {
    Pnt *point = ptst + i;
    Pnt *parent = ptss + point->parent;

    if (!parent->isGood || parent->lastHessian < 0.1)
      continue;
    if (!point->isGood) {
      point->iR = point->idepth = point->idepth_new = parent->iR;
      point->isGood = true;
      point->lastHessian = 0;
    } else {
      float newiR = (point->iR * point->lastHessian * 2 +
                     parent->iR * parent->lastHessian) /
                    (point->lastHessian * 2 + parent->lastHessian);
      point->iR = point->idepth = point->idepth_new = newiR;
    }
  }
  optReg(srcLvl - 1);
}

void CoarseInitializer::setFirst(CalibHessian *HCalib,
                                 FrameHessian *newFrameHessian) {

  makeK(HCalib);
  firstFrame = newFrameHessian;

  PixelSelector sel(w_[0], h_[0]);

  float *statusMap = new float[w_[0] * h_[0]];
  bool *statusMapB = new bool[w_[0] * h_[0]];

  float densities[] = {0.03, 0.05, 0.15, 0.5, 1};
  for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
    sel.currentPotential = 3;
    int npts;
    if (lvl == 0)
      npts = sel.makeMaps(firstFrame, statusMap, densities[lvl] * w_[0] * h_[0],
                          1, false, 2);
    else
      npts = makePixelStatus(firstFrame->dIp[lvl], statusMapB, w_[lvl], h_[lvl],
                             densities[lvl] * w_[0] * h_[0]);

    if (points[lvl] != 0)
      delete[] points[lvl];
    points[lvl] = new Pnt[npts];

    // set idepth map to initially 1 everywhere.
    int wl = w_[lvl], hl = h_[lvl];
    Pnt *pl = points[lvl];
    int nl = 0;
    for (int y = patternPadding + 1; y < hl - patternPadding - 2; y++)
      for (int x = patternPadding + 1; x < wl - patternPadding - 2; x++) {
        // if(x==2) printf("y=%d!\n",y);
        if ((lvl != 0 && statusMapB[x + y * wl]) ||
            (lvl == 0 && statusMap[x + y * wl] != 0)) {
          // assert(patternNum==9);
          pl[nl].u = x + 0.1;
          pl[nl].v = y + 0.1;
          pl[nl].idepth = 1;
          pl[nl].iR = 1;
          pl[nl].isGood = true;
          pl[nl].energy.setZero();
          pl[nl].lastHessian = 0;
          pl[nl].lastHessian_new = 0;
          pl[nl].my_type = (lvl != 0) ? 1 : statusMap[x + y * wl];

          Eigen::Vector3f *cpt = firstFrame->dIp[lvl] + x + y * w_[lvl];
          float sumGrad2 = 0;
          for (int idx = 0; idx < patternNum; idx++) {
            int dx = patternP[idx][0];
            int dy = patternP[idx][1];
            float absgrad = cpt[dx + dy * w_[lvl]].tail<2>().squaredNorm();
            sumGrad2 += absgrad;
          }

          //				float gth = setting_outlierTH *
          //(sqrtf(sumGrad2)+setting_outlierTHSumComponent);
          // pl[nl].outlierTH = patternNum*gth*gth;
          //

          pl[nl].outlierTH = patternNum * setting_outlierTH;

          nl++;
          assert(nl <= npts);
        }
      }

    numPoints[lvl] = nl;
  }
  delete[] statusMap;
  delete[] statusMapB;

  makeNN();

  thisToNext = SE3();
  snapped = false;
  frameID = snappedAt = 0;
}

void CoarseInitializer::resetPoints(int lvl) {
  Pnt *pts = points[lvl];
  int npts = numPoints[lvl];
  for (int i = 0; i < npts; i++) {
    pts[i].energy.setZero();
    pts[i].idepth_new = pts[i].idepth;

    if (lvl == pyrLevelsUsed - 1 && !pts[i].isGood) {
      float snd = 0, sn = 0;
      for (int n = 0; n < 10; n++) {
        if (pts[i].neighbours[n] == -1 || !pts[pts[i].neighbours[n]].isGood)
          continue;
        snd += pts[pts[i].neighbours[n]].iR;
        sn += 1;
      }

      if (sn > 0) {
        pts[i].isGood = true;
        pts[i].iR = pts[i].idepth = pts[i].idepth_new = snd / sn;
      }
    }
  }
}
void CoarseInitializer::doStep(int lvl, float lambda, Vec8f inc) {

  const float maxPixelStep = 0.25;
  const float idMaxStep = 1e10;
  Pnt *pts = points[lvl];
  int npts = numPoints[lvl];
  for (int i = 0; i < npts; i++) {
    if (!pts[i].isGood)
      continue;

    float b = JbBuffer[i][8] + JbBuffer[i].head<8>().dot(inc);
    float step = -b * JbBuffer[i][9] / (1 + lambda);

    float maxstep = maxPixelStep * pts[i].maxstep;
    if (maxstep > idMaxStep)
      maxstep = idMaxStep;

    if (step > maxstep)
      step = maxstep;
    if (step < -maxstep)
      step = -maxstep;

    float newIdepth = pts[i].idepth + step;
    if (newIdepth < 1e-3)
      newIdepth = 1e-3;
    if (newIdepth > 50)
      newIdepth = 50;
    pts[i].idepth_new = newIdepth;
  }
}
void CoarseInitializer::applyStep(int lvl) {
  Pnt *pts = points[lvl];
  int npts = numPoints[lvl];
  for (int i = 0; i < npts; i++) {
    if (!pts[i].isGood) {
      pts[i].idepth = pts[i].idepth_new = pts[i].iR;
      continue;
    }
    pts[i].energy = pts[i].energy_new;
    pts[i].isGood = pts[i].isGood_new;
    pts[i].idepth = pts[i].idepth_new;
    pts[i].lastHessian = pts[i].lastHessian_new;
  }
  std::swap<Vec10f *>(JbBuffer, JbBuffer_new);
}

void CoarseInitializer::makeNN() {
  const float NNDistFactor = 0.05;

  typedef nanoflann::KDTreeSingleIndexAdaptor<
      nanoflann::L2_Simple_Adaptor<float, FLANNPointcloud>, FLANNPointcloud, 2>
      KDTree;

  // build indices
  FLANNPointcloud pcs[PYR_LEVELS];
  KDTree *indexes[PYR_LEVELS];
  for (int i = 0; i < pyrLevelsUsed; i++) {
    pcs[i] = FLANNPointcloud(numPoints[i], points[i]);
    indexes[i] =
        new KDTree(2, pcs[i], nanoflann::KDTreeSingleIndexAdaptorParams(5));
    indexes[i]->buildIndex();
  }

  const int nn = 10;

  // find NN & parents
  for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
    Pnt *pts = points[lvl];
    int npts = numPoints[lvl];

    int ret_index[nn];
    float ret_dist[nn];
    nanoflann::KNNResultSet<float, int, int> resultSet(nn);
    nanoflann::KNNResultSet<float, int, int> resultSet1(1);

    for (int i = 0; i < npts; i++) {
      // resultSet.init(pts[i].neighbours, pts[i].neighboursDist );
      resultSet.init(ret_index, ret_dist);
      Vec2f pt = Vec2f(pts[i].u, pts[i].v);
      indexes[lvl]->findNeighbors(resultSet, (float *)&pt,
                                  nanoflann::SearchParams());
      int myidx = 0;
      float sumDF = 0;
      for (int k = 0; k < nn; k++) {
        pts[i].neighbours[myidx] = ret_index[k];
        float df = expf(-ret_dist[k] * NNDistFactor);
        sumDF += df;
        pts[i].neighboursDist[myidx] = df;
        assert(ret_index[k] >= 0 && ret_index[k] < npts);
        myidx++;
      }
      for (int k = 0; k < nn; k++)
        pts[i].neighboursDist[k] *= 10 / sumDF;

      if (lvl < pyrLevelsUsed - 1) {
        resultSet1.init(ret_index, ret_dist);
        pt = pt * 0.5f - Vec2f(0.25f, 0.25f);
        indexes[lvl + 1]->findNeighbors(resultSet1, (float *)&pt,
                                        nanoflann::SearchParams());

        pts[i].parent = ret_index[0];
        pts[i].parentDist = expf(-ret_dist[0] * NNDistFactor);

        assert(ret_index[0] >= 0 && ret_index[0] < numPoints[lvl + 1]);
      } else {
        pts[i].parent = -1;
        pts[i].parentDist = -1;
      }
    }
  }

  // done.

  for (int i = 0; i < pyrLevelsUsed; i++)
    delete indexes[i];
}
} // namespace dso
