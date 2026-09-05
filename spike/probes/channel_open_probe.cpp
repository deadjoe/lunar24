// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
// P1 slice-1 disposable probe (spike/). See FINDINGS.md. Not part of the product.
#include "RtAudio.h"
#include <cstdio>
static int cb(void* outBuf, void* , unsigned int nFrames, double, unsigned int, void*) {
  if (outBuf) for (unsigned int i=0;i<nFrames;i++) ((double*)outBuf)[i]=0.0;
  return 0;
}
void t(RtAudio& rta, unsigned int dev, unsigned int ch, const char* label) {
  RtAudio::StreamParameters o; o.deviceId=dev; o.nChannels=ch; o.firstChannel=0;
  RtAudio::StreamOptions opts;
  unsigned int bufSize = 0;
  RtAudioErrorType e = rta.openStream(&o, nullptr, RTAUDIO_FLOAT64, 48000, &bufSize, &cb, nullptr, &opts);
  printf("  %-20s ch=%u open=%d(%s) bufSize=%u\n", label, ch, (int)e,
         e==RTAUDIO_NO_ERROR?"OK":"FAIL", bufSize); fflush(stdout);
  if (e==RTAUDIO_NO_ERROR) { RtAudioErrorType s=rta.startStream();
    printf("      start=%d(%s)\n", (int)s, s==RTAUDIO_NO_ERROR?"OK":"FAIL");
    if (s==RTAUDIO_NO_ERROR) rta.stopStream(); rta.closeStream(); }
}
int main() {
  setbuf(stdout,NULL);
  RtAudio rta;
  printf("=== 2-out Mac mini Speakers (131) native=2 ===\n");
  t(rta, 131, 2, "want 2 (native)");
  t(rta, 131, 4, "want 4 (host decl)");
  t(rta, 131, 8, "want 8 (over)");
  printf("=== 8-out Studio Display (130) native=8 ===\n");
  t(rta, 130, 2, "want 2");
  t(rta, 130, 4, "want 4 (host decl)");
  t(rta, 130, 8, "want 8 (native)");
  t(rta, 130, 12, "want 12 (over)");
  return 0;
}
