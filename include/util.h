//
// Created by Chris Kjellqvist on 2/23/23.
//

#ifndef COMPOSERRUNTIME_UTIL_H
#define COMPOSERRUNTIME_UTIL_H

constexpr int roundUp(float q) {
  float d = q - (int) q;
  if (q < 0) {
    return q - d;
  } else {
    return q + d;
  }
}

#endif //COMPOSERRUNTIME_UTIL_H
