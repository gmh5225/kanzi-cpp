/*
Copyright 2011-2021 Frederic Langlet
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

#ifndef _BlockDecompressor_
#define _BlockDecompressor_

#include <map>
#include <vector>

#include "../concurrent.hpp"
#include "../Context.hpp"
#include "../OutputStream.hpp"
#include "../Listener.hpp"
#include "../io/CompressedInputStream.hpp"

namespace kanzi {
   class FileDecompressResult {
   public:
       int _code;
       uint64 _read;
       std::string _errMsg;

       FileDecompressResult()
       {
           _code = 0;
           _read = 0;
           _errMsg = "";
       }

       FileDecompressResult(int code, uint64 read, const std::string& errMsg)
       {
           _code = code;
           _read = read;
           _errMsg = errMsg;
       }

       ~FileDecompressResult() {}
   };

#ifdef CONCURRENCY_ENABLED
   template <class T, class R>
   class FileDecompressWorker : public Task<R> {
   public:
       FileDecompressWorker(BoundedConcurrentQueue<T>* queue) { _queue = queue; }

       ~FileDecompressWorker() {}

       R run();

   private:
       BoundedConcurrentQueue<T>* _queue;
   };
#endif

   template <class T>
   class FileDecompressTask : public Task<T> {
   public:
       static const int DEFAULT_BUFFER_SIZE = 65536;

       FileDecompressTask(Context& ctx, std::vector<Listener*>& listeners);

       ~FileDecompressTask();

       T run();

       void dispose();

   private:
       Context _ctx;
       OutputStream* _os;
       CompressedInputStream* _cis;
       std::vector<Listener*> _listeners;
   };

   class BlockDecompressor {
       friend class FileDecompressTask<FileDecompressResult>;

   public:
       BlockDecompressor(std::map<std::string, std::string>& map);

       ~BlockDecompressor();

       int decompress(uint64& read);

       bool addListener(Listener& bl);

       bool removeListener(Listener& bl);

       void dispose();

   private:
       static const int DEFAULT_BUFFER_SIZE = 32768;
       static const int DEFAULT_CONCURRENCY = 1;
       static const int MAX_CONCURRENCY = 64;

       int _verbosity;
       bool _overwrite;
       std::string _inputName;
       std::string _outputName;
       int _blockSize;
       int _jobs;
       int _from; // start block
       int _to; // end block
       std::vector<Listener*> _listeners;

       static void notifyListeners(std::vector<Listener*>& listeners, const Event& evt);
   };
}
#endif
