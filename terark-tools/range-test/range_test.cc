#include <util/sync_point.h>
#include "range_test.h"

namespace rocksdb {
class RangeTest {
 public:
  RangeTest(const std::string &conf_path, const std::string &&name,
          uint64_t record_count)
      : config_file_(conf_path), db_name_(name), count_(record_count) {
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;

    rocksdb::Status s = rocksdb::LoadOptionsFromFile(
        config_file_, Env::Default(), &db_options_, &cf_descs, false);
    if (!s.ok()) {
      fprintf(stderr, "Load Option Error! %s\n", s.getState());
      assert(false);
    }
    assert(cf_descs.size() == 1);

    cf_descs[0].options.enable_lazy_compaction = FLAGS_lazy;
    cf_descs[0].options.disable_auto_compactions = true;
    cf_descs[0].options.num_levels = 2;
    cf_descs[0].options.target_file_size_base = 2 * 1048576;  // 4Mb

    // cf_descs[0].options.compression = kNoCompression;
    // cf_descs[0].options.compression_per_level = {kNoCompression,
    //                                             kNoCompression};
    cf_desc_ = cf_descs[0];
    cf_desc_.options.table_factory.reset(
        rocksdb::NewBlockBasedTableFactory(BlockBasedTableOptions()));
    if (FLAGS_table_factory.compare("TerarkZipTable") == 0) {
      cf_desc_.options.table_factory.reset(rocksdb::NewTerarkZipTableFactory(
          TerarkZipTableOptions(), cf_desc_.options.table_factory));
      db_options_.allow_mmap_reads = true;
    }
    std::cout << cf_desc_.options.table_factory->Name() << std::endl;

    OpenDB();
  };

  void LoadData() {
    auto put_one_record = [&]() {
      std::string key, value;
      auto fill_kv = [&](std::string &k, std::string &v) {
        auto kdata =
            std::uniform_int_distribution<uint32_t>(0, FLAGS_record_count);
        key.assign(std::to_string(kdata(mt)));

        auto vlen = std::uniform_int_distribution<size_t>(64, 512);
        v.resize(vlen(mt));
        auto gen_char = [&]() {
          auto uid = std::uniform_int_distribution<char>(0, 255);
          return uid(mt);
        };
        gen_char();
        std::generate_n(v.begin(), v.size(), gen_char);
      };

      while (count_.load() > 0) {
        fill_kv(key, value);
        auto s =
            db_->Put(WriteOptions(), cf_handles_[0], Slice(key), Slice(value));
        if (!s.ok()) {
          fprintf(stderr, "Open Error! %s\n", s.getState());
          assert(false);
        }
        count_.fetch_sub(1);
      }
    };

    uint32_t thread_num = 8;
    std::vector<std::thread> thread_vec;
    for (int j = 0; j < thread_num; ++j) {
      thread_vec.emplace_back(put_one_record);
    }
    for (auto &t : thread_vec) {
      t.join();
    }
    db_->Flush(FlushOptions());

    std::cout << "Load " << FLAGS_record_count << " records." << std::endl;
  }

  void LoadDataV2() {
    std::atomic<int> record_in_sst;
    auto num = std::uniform_int_distribution<int>(
        FLAGS_record_per_table - 10, FLAGS_record_per_table + 10)(mt);

    for (int i = 0; i < FLAGS_table_merge_num; i++) {
      record_in_sst.store(num);
      auto put_one_sst = [&]() {
        auto k_start =
            std::uniform_int_distribution<uint32_t>(0, FLAGS_key_range)(mt);
        auto k_end = std::uniform_int_distribution<uint32_t>(
            k_start + FLAGS_key_range / FLAGS_table_merge_num,
            FLAGS_key_range)(mt);

        std::string key, value;
        auto fill_kv = [&](std::string &k, std::string &v) {
          auto kdata = std::uniform_int_distribution<uint32_t>(k_start, k_end);
          key.assign(std::to_string(kdata(mt)));

          auto vlen = std::uniform_int_distribution<size_t>(
              FLAGS_val_avg_size - 1000, FLAGS_val_avg_size + 1000);
          v.resize(vlen(mt));
          auto gen_char = [&]() {
            auto uid = std::uniform_int_distribution<char>(0, 255);
            return uid(mt);
          };
          gen_char();
          std::generate_n(v.begin(), v.size(), gen_char);
        };

        while (record_in_sst.load() > 0) {
          fill_kv(key, value);
          auto s = db_->Put(WriteOptions(), cf_handles_[0], Slice(key),
                            Slice(value));
          if (!s.ok()) {
            fprintf(stderr, "Open Error! %s\n", s.getState());
            assert(false);
          }
          record_in_sst.fetch_sub(1);
        }
      };

      std::vector<std::thread> thread_vec;
      for (int j = 0; j < FLAGS_load_threads; ++j) {
        thread_vec.emplace_back(put_one_sst);
      }
      for (auto &t : thread_vec) {
        t.join();
      }
      db_->Flush(FlushOptions());
      std::cout << "FLush: " << i << std::endl;
    }
  }

  void CompactRange() {
    std::cout << "CompactRange Started" << std::endl;
    if (FLAGS_use_user_range) {
      std::string begin_key(std::to_string(0));
      std::string end_key(std::to_string(FLAGS_record_count));
      Slice begin_key_s(begin_key);
      Slice end_key_s(end_key);

      auto iter = db_->NewIterator(ReadOptions());
      iter->SeekToFirst();

      auto s =
          db_->CompactRange(CompactRangeOptions(), &begin_key_s, &end_key_s);
      if (!s.ok()) {
        fprintf(stderr, "CompactRange Error! %s\n", s.getState());
        assert(false);
      }
      return;
    } else if (FLAGS_use_seek_range) {
      auto iter = db_->NewIterator(ReadOptions());
      iter->SeekToFirst();
      auto begin_key = iter->key();
      iter->SeekToLast();
      auto end_key = iter->key();

      auto s = db_->CompactRange(CompactRangeOptions(), &begin_key, &end_key);
      if (!s.ok()) {
        fprintf(stderr, "CompactRange Error! %s\n", s.getState());
        assert(false);
      }
      return;
    } else {
      auto s = db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
      if (!s.ok()) {
        fprintf(stderr, "CompactRange Error! %s\n", s.getState());
        assert(false);
      }
    }
    std::cout << "CompactRange Finished" << std::endl;
  }

  void PrintStat() {
    auto last_get_found = get_found_.load();
    auto last_get_miss = get_miss_.load();

    while (true) {
      auto cur_get_found = get_found_.load();
      auto cur_get_miss = get_miss_.load();
      std::cout << "Get Found: " << cur_get_found - last_get_found;
      std::cout << ", Get Missed: " << cur_get_miss - last_get_miss;
      std::cout << ", Get QPS: "
                << (cur_get_found - last_get_found + cur_get_miss -
                    last_get_miss) /
                       60;
      std::cout << std::endl;

      last_get_miss = cur_get_miss;
      last_get_found = cur_get_found;

      std::this_thread::sleep_for(std::chrono::seconds(60));
    }
  }

  void ReadFunc() {
    std::string rnd_key;
    while (true) {
      auto kdata =
          std::uniform_int_distribution<uint32_t>(0, FLAGS_record_count);
      rnd_key.assign(std::to_string(kdata(mt)));

      std::string value;
      auto s = db_->Get(ReadOptions(), Slice(rnd_key), &value);
      if (!s.ok()) {
        get_miss_.fetch_add(1);
      }
      get_found_.fetch_add(1);
    }
  }

 private:
  void OpenDB() {
    rocksdb::Status s;
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;

    cf_descs.push_back(cf_desc_);
    db_options_.create_if_missing = true;
    db_options_.create_missing_column_families = true;
    s = DB::Open(db_options_, db_name_, cf_descs, &cf_handles_, &db_);
    if (!s.ok()) {
      fprintf(stderr, "Open Error! %s\n", s.getState());
      assert(false);
    }
    assert(cf_handles_.size() == 1);
  }

  std::string config_file_ = "";
  DB *db_;
  std::string db_name_ = "";
  std::vector<rocksdb::ColumnFamilyHandle *> cf_handles_;  // size = 1
  std::atomic<int64_t> count_;
  std::atomic<uint64_t> get_found_{0};
  std::atomic<uint64_t> get_miss_{0};

  std::random_device rd;
  std::mt19937_64 mt{rd()};

  DBOptions db_options_;
  ColumnFamilyDescriptor cf_desc_;
};

}  // namespace rocksdb

int main(int argc, char *argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MapBuilder::Build::build_map_sst",
      [](void *ptr) { *(bool *)ptr = true; });
  if (FLAGS_disable_force_memory) {
    rocksdb::SyncPoint::GetInstance()->SetCallBack(
        "MapBuilder::Build::force_memory",
        [](void *ptr) { *(bool *)ptr = false; });
  }

  rocksdb::RangeTest t("./db.ini", "./mapdb", FLAGS_record_count);

  t.LoadDataV2();

  t.CompactRange();

  std::thread qps_watcher(&rocksdb::RangeTest::PrintStat, &t);
  t.ReadFunc();
  return 0;
}