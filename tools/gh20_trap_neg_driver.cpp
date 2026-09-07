// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gh20_trap_neg_driver.cpp — task #88 (GH #20) negative/acceptance discriminator driver.
//
// Drives the PRODUCTION PolivoksFilter directly (the same class the real codec->
// owner->render path instantiates) at a small grid of (sr, mode, res, norm, freq)
// cells and measures, per cell, the LS-fit matched-sinusoid (fundamental) RMS and
// the per-cell residual noise floor, plus the LP DC gain at res=0 and res=1 and a
// BP DC-block reading. The shadow-included polivoks_vcf.h carries a MUTATION; this
// driver is recompiled once per mutation so the measurement sees the mutation.
//
// The orchestrator (run_gh20_trap_negatives.py) compares these measurements to the
// independent bilinear reference (gh20_trap_reference) and asserts each mutation
// exceeds the 0.1 dB bound (or triggers its specific L/R or DC identity) while the
// benign shadow copy of the REAL header stays GREEN. Header-only (INTERFACE) class,
// no host link needed — keeps each negative build and run to a few seconds.
//
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>
#include "polivoks_vcf.h"
using namespace lunar24::core;

static const size_t kWarm = 24576;             // settle the Q~10 resonator (res=1, high sr).
static const size_t kWin = 16384;
static const double kPi = 3.14159265358979323846;

// LS-fit fundamental projection (solve the 2x2 Gram / normal equations). Returns
// (amplitude_rms, noise_rms). The single-frequency matched-sinusoid magnitude is
// sqrt(0.5*(a^2+b^2)); the residual (orthogonal fit residue) is the noise floor.
static std::pair<double,double> fit(const std::vector<double>& c, size_t begin, double freq, double sr) {
  const size_t n = c.size() - begin;
  double c11=0,c12=0,c22=0,r1=0,r2=0,e=0;
  const double w = 2.0*kPi*freq/sr;
  for (size_t i=begin;i<c.size();++i){
    double ph=w*double(i-begin); double co=std::cos(ph),si=std::sin(ph);
    c11+=co*co; c12+=co*si; c22+=si*si; r1+=c[i]*co; r2+=c[i]*si; e+=c[i]*c[i];
  }
  double det=c11*c22-c12*c12;
  if (det<=1e-15) return {0.0,0.0};
  double a=(r1*c22-r2*c12)/det, b=(r2*c11-r1*c12)/det;
  double rese=e-(a*r1+b*r2); if (rese<0.0) rese=0.0;
  return { std::sqrt(0.5*(a*a+b*b)), std::sqrt(rese/ double(n)) };
}

static double dc_avg(const std::vector<double>& c, size_t begin) {
  double s=0.0; size_t cnt=0;
  for (size_t i=begin;i<c.size();++i){ s+=c[i]; ++cnt; }
  return cnt? s/double(cnt) : 0.0;
}

int main(){
  const double lvl = 0.05;
  const double f_list[] = {60,100,200,400,800,1600,2500,4000,8000};
  const double norm_list[] = {0.15,0.5,0.9,1.0};
  const double res_list[] = {0.0,0.5,1.0};
  const double sr_list[] = {44100,48000,88200,96000};
  const char* mode_list[] = {"lp","bp"};

  // tsv header (id-style columns the orchestrator reads).
  printf("cell\tmode\tres\tnorm\tlvl\tfreq_hz\tsr_hz\tchannel\tamp_rms\tnoise_rms\tok\n");

  for (double sr : sr_list)
  for (auto mode : mode_list)
  for (double res : res_list)
  for (double norm : norm_list)
  for (double f : f_list){
    PolivoksFilter flt;
    flt.setSampleRate(sr);
    flt.setMode(0, mode[0]=='b');
    flt.setFreq(0, norm); flt.setRes(0, res);
    std::vector<double> out(kWarm+kWin, 0.0);
    for (size_t i=0;i<kWarm+kWin;++i){
      double t=double(i)/sr;
      double in = lvl*std::sin(2.0*kPi*f*t);
      double ol,orr; flt.process(in, 0.0, ol, orr);
      out[i]=ol;
    }
    auto fa = fit(out, kWarm, f, sr);
    printf("sig\t%s\t%g\t%g\t%g\t%g\t%g\twetL\t%.9g\t%.9g\t1\n",
           mode, res, norm, lvl, f, sr, fa.first, fa.second);
  }

  // L/R isolation: both channels, same sine, chan0 at norm, chan1 at 0.3 (a DIFFERENT
  // cutoff). If L/R state is crossed, wetR collapses toward wetL -> the two outputs match.
  for (double sr : sr_list) for (double f : f_list) for (double norm : norm_list) {
    PolivoksFilter flt; flt.setSampleRate(sr);
    flt.setMode(0,false); flt.setFreq(0,norm); flt.setRes(0,0.0);   // lp
    flt.setMode(1,false); flt.setFreq(1,0.3); flt.setRes(1,0.0);    // lp @0.3
    std::vector<double> oL(kWarm+kWin,0), oR(kWarm+kWin,0);
    for (size_t i=0;i<kWarm+kWin;++i){
      double t=double(i)/sr; double in=lvl*std::sin(2.0*kPi*f*t);
      double ol,orr; flt.process(in,in,ol,orr); oL[i]=ol; oR[i]=orr;
    }
    auto aL=fit(oL,kWarm,f,sr), aR=fit(oR,kWarm,f,sr);
    printf("lr\tlp\t0\t%g\t%g\t%g\t%g\twetL\t%.9g\t%.9g\t1\n", norm, lvl, f, sr, aL.first, aL.second);
    printf("lr\tlp\t0\t0.3\t%g\t%g\t%g\twetR\t%.9g\t%.9g\t1\n", lvl, f, sr, aR.first, aR.second);
  }

  // LP DC gain at res=0 and res=1, plus BP DC-block. 0 dB & -inf expected for LP & BP.
  for (double sr : sr_list) for (double res : res_list) {
    PolivoksFilter lp; lp.setSampleRate(sr); lp.setMode(0,false); lp.setFreq(0,0.5); lp.setRes(0,res);
    std::vector<double> o(kWarm+kWin,0.0);
    for (size_t i=0;i<kWarm+kWin;++i){ double ol,orr; lp.process(lvl,0.0,ol,orr); o[i]=ol; }
    double dc=dc_avg(o,kWarm);
    printf("dc\tlp\t%g\t0.5\t%g\t0\t%g\twetL\t%.9g\t%.9g\t%d\n", res, lvl, sr, dc, 0.0, dc>0?1:0);
    PolivoksFilter bp; bp.setSampleRate(sr); bp.setMode(0,true); bp.setFreq(0,0.5); bp.setRes(0,res);
    std::vector<double> ob(kWarm+kWin,0.0);
    for (size_t i=0;i<kWarm+kWin;++i){ double ol,orr; bp.process(lvl,0.0,ol,orr); ob[i]=ol; }
    double dcb=dc_avg(ob,kWarm);
    printf("dc\tbp\t%g\t0.5\t%g\t0\t%g\twetL\t%.9g\t%.9g\t%d\n", res, lvl, sr, dcb, 0.0, 1);
  }
  return 0;
}
