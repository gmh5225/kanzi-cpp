/*
Copyright 2011-2022 Frederic Langlet
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
you may obtain a copy of the License at

                http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once
#ifndef _CMPredictor_
#define _CMPredictor_

#include "../Predictor.hpp"

namespace kanzi
{

   class CMPredictor : public Predictor
   {
   private:
       static const int FAST_RATE = 2;
       static const int MEDIUM_RATE = 4;
       static const int SLOW_RATE = 6;
       static const int PSCALE = 65536;

       int _c1;
       int _c2;
       int _ctx;
       int _runMask;
       int _counter1[256][257];
       int _counter2[512][17];
       int* _pc1;
       int* _pc2;

   public:
       CMPredictor();

       ~CMPredictor(){}

       void update(int bit);

       int get();
   };

   // Update the probability model
   inline void CMPredictor::update(int bit)
   {
       _ctx <<= 1;

       if (bit == 0) {
           _pc1[256] -= (_pc1[256] >> FAST_RATE);
           _pc1[_c1] -= (_pc1[_c1] >> MEDIUM_RATE);
           _pc2[0] -= (_pc2[0]>> SLOW_RATE);
           _pc2[1] -= (_pc2[1]>> SLOW_RATE);
       }
       else {
           _pc1[256] -= ((_pc1[256] - PSCALE + 16) >> FAST_RATE);
           _pc1[_c1] -= ((_pc1[_c1] - PSCALE + 16) >> MEDIUM_RATE);
           _pc2[0] -= ((_pc2[0] - PSCALE + 16) >> SLOW_RATE);
           _pc2[1] -= ((_pc2[1] - PSCALE + 16) >> SLOW_RATE);
           _ctx++;
       }

       if (_ctx > 255) {
           _c2 = _c1;
           _c1 = _ctx & 0xFF;
           _ctx = 1;
           _runMask = (_c1 == _c2) ? 0x100 : 0;
       }
   }

   // Return the split value representing the probability of 1 in the [0..4095] range.
   inline int CMPredictor::get()
   {
       _pc1 = _counter1[_ctx];
       const int p = (13 * (_pc1[256] + _pc1[_c1]) + 6 * _pc1[_c2]) >> 5;
       _pc2 = &_counter2[_ctx | _runMask][p >> 12];
       const int x1 = _pc2[0];
       const int x2 = _pc2[1];
       const int ssep = x1 + (((x2 - x1) * (p & 4095)) >> 12);
       return (p + 3*ssep + 32) >> 6; // rescale to [0..4095]
   }
}
#endif