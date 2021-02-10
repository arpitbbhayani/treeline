#include <iostream>
#include <vector>

#include "db/memtable.h"
#include "gflags/gflags.h"
#include "rs/builder.h"
#include "util/inlineskiplist.h"
#include "ycsbr/ycsbr.h"

namespace {

DEFINE_string(load_path, "", "Path to the bulk load workload file.");
DEFINE_string(workload_path, "", "Path to the workload file.");
DEFINE_uint32(record_size_bytes, 16, "Size of a database record in bytes.");
DEFINE_uint32(
    page_fill_pct, 50,
    "How full each page should be after bulk loading, in percentage points.");
DEFINE_uint64(page_size, 64 * 1024, "The size of a page in bytes.");
DEFINE_uint64(memtable_flush_threshold, 64 * 1024 * 1024,
              "The threshold above which the memtable is flushed, in bytes.");

DEFINE_uint64(
    io_threshold, 1,
    "The minimum number of operations to a given page that need to be "
    "encoutered while flushing a memtable in order to trigger a flush");
DEFINE_uint64(max_deferrals, 0,
              "The maximum number of times that a given operation can be "
              "deferred to a future flush.");

}  // namespace

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage("Determine the impact of deferring I/O");
  gflags::ParseCommandLineFlags(&argc, &argv, /*remove_flags=*/true);

  if (FLAGS_load_path.empty()) {
    std::cerr << "ERROR: Please provide a bulk load workload." << std::endl;
    return 1;
  }
  if (FLAGS_workload_path.empty()) {
    std::cerr << "ERROR: Please provide a workload." << std::endl;
    return 1;
  }

  // Obtain and process the bulk load workload.
  ycsbr::Workload::Options loptions;
  loptions.value_size = FLAGS_record_size_bytes - 8;
  loptions.sort_requests = true;
  loptions.swap_key_bytes = false;
  ycsbr::BulkLoadWorkload load =
      ycsbr::BulkLoadWorkload::LoadFromFile(FLAGS_load_path, loptions);
  auto minmax = load.GetKeyRange();
  const size_t num_keys = load.size();

  // Initialize a RadixSpline.
  rs::Builder<uint64_t> rsb(__builtin_bswap64(minmax.min),
                            __builtin_bswap64(minmax.max));
  for (const auto& req : load) rsb.AddKey(__builtin_bswap64(req.key));
  const rs::RadixSpline<uint64_t> rs = rsb.Finalize();

  // Calculate records per page and number of pages.
  const double fill_pct = FLAGS_page_fill_pct / 100.;
  const size_t records_per_page =
      FLAGS_page_size * fill_pct / (FLAGS_record_size_bytes + 10);
  size_t num_pages = num_keys / records_per_page;
  if (num_keys % records_per_page != 0) ++num_pages;

  // Open workload.
  ycsbr::Workload::Options options;
  options.value_size = FLAGS_record_size_bytes - 8;
  ycsbr::Workload workload =
      ycsbr::Workload::LoadFromFile(FLAGS_workload_path, options);

  // Bookkeeping.
  std::vector<size_t> memtable_entries_per_page(num_pages, 0);
  std::vector<uint64_t> page_deferral_count(num_pages, 0);
  std::vector<bool> flushed_this_time(, num_pages, false);
  llsm::MemTable* memtable = new llsm::MemTable();
  llsm::MemTable* backup_memtable = new llsm::MemTable();
  size_t num_flushes = 0;
  size_t num_ios = 0;
  size_t num_reqs = 0;
  size_t num_inserts = 0;

  // Process the workload.
  for (const auto& req : workload) {
    ++num_reqs;
    if (req.op == ycsbr::Request::Operation::kRead ||
        req.op == ycsbr::Request::Operation::kScan)
      continue;
    ++num_inserts;

    // Perform the insert
    memtable->Add(llsm::Slice(reinterpret_cast<const char*>(&req.key), 8),
                  llsm::Slice(req.value, 8), llsm::MemTable::EntryType::kWrite);
    const size_t insert_page_id =
        rs.GetEstimatedPosition(__builtin_bswap64(req.key)) / records_per_page;
    ++memtable_entries_per_page[insert_page_id];

    // Check if the memtable is large enough to flush.
    if (memtable->ApproximateMemoryUsage() >= FLAGS_memtable_flush_threshold) {
      ++num_flushes;

      auto it = memtable->GetIterator();
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        const size_t page_id =
            rs.GetEstimatedPosition(__builtin_bswap64(
                *reinterpret_cast<const uint64_t*>(it.key().data()))) /
            records_per_page;
        if (memtable_entries_per_page[page_id] >= FLAGS_io_threshold ||
            page_deferral_count[page_id] >= FLAGS_max_deferrals) {
          flushed_this_time[page_id] = true;
        } else {
          backup_memtable->Add(it.key(), it.value(), it.type());
        }
      }

      for (size_t i = 0; i < num_pages; ++i) {
        if (flushed_this_time[i]) {
          ++num_ios;
          memtable_entries_per_page[i] = 0;
          page_deferral_count[i] = 0;
          flushed_this_time[i] = false;
        } else {
          ++page_deferral_count[i];
        }
      }

      // Swap memtables
      delete memtable;
      memtable = backup_memtable;
      backup_memtable = new llsm::MemTable();
    }
  }

  // Flush what's remaining.
  ++num_flushes;
  for (size_t i = 0; i < num_pages; ++i) {
    if (memtable_entries_per_page[i] > 0) ++num_ios;
  }

  delete memtable;
  delete backup_memtable;

  // Print statistics
  std::cout << "-------------------------------" << std::endl;
  std::cout << "Parameters used: " << std::endl;
  std::cout << "\tLoad path: " << FLAGS_load_path << std::endl;
  std::cout << "\tWorkload path: " << FLAGS_workload_path << std::endl;
  std::cout << "\tRecord size (bytes): " << FLAGS_record_size_bytes
            << std::endl;
  std::cout << "\n\tPage fill percentage: " << FLAGS_page_fill_pct << std::endl;
  std::cout << "\tPage size (bytes): " << FLAGS_page_size << std::endl;
  std::cout << "\n\tMemtable flush threshold (bytes): "
            << FLAGS_memtable_flush_threshold << std::endl;
  std::cout << "\n\tMin requests for I/O: " << FLAGS_io_threshold << std::endl;
  std::cout << "\tMax number of deferrals: " << FLAGS_max_deferrals
            << std::endl;

  std::cout << "Results: " << std::endl;
  std::cout << "\tNum keys: " << num_keys << std::endl;
  std::cout << "\tNum requests processed: " << num_reqs << std::endl;
  std::cout << "\tNum inserts processed: " << num_inserts << std::endl;
  std::cout << "\tNum pages used: " << num_pages << std::endl;
  std::cout << "\n\tNum times memtable was flushed: " << num_flushes
            << std::endl;
  std::cout << "\tNum of I/Os caused by flushes: " << num_ios << std::endl;
  std::cout << "-------------------------------" << std::endl;
}
