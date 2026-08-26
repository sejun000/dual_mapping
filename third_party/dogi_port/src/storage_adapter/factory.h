#include <string>
#include <iostream>
#include "src/storage_adapter/storage_adapter.h"
#include "src/storage_adapter/local_adapter.h"
// Port: ZenFS adapter removed (no ZNS device)
//#include "src/storage_adapter/hdfs_adapter.h"
//#include "src/storage_adapter/zoned_adapter.h"

class StorageAdapterFactory {
  public:
    class NullAdapter : public StorageAdapter {   // metadata-only run: payloads are never needed for WAF
      public:
        void Write(const void *, int, off64_t) override {}
        void CreateSegment(int) override {}
        void Read(void *, int, off64_t) override {}
        void ReadWholeSegment(void *, int) override {}
        void DestroySegment(int) override {}
    };
    static StorageAdapter *GetInstance(std::string type) {
      if (type == "Local") {
        return new LocalAdapter();
      } else if (type == "Null") {
        return new NullAdapter();
      } else {
        std::cerr << "No StorageAdapter, type: " << type << std::endl;
      }
      return new NullAdapter();
    }
};
