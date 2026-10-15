// Runs every file in the given directories through the fuzz target once.
// Used by ctest so saved crash inputs stay fixed, on any compiler (no
// libFuzzer needed).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
  std::size_t n = 0;
  for (int i = 1; i < argc; ++i) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(argv[i])) {
      if (!e.is_regular_file()) continue;
      std::ifstream f(e.path(), std::ios::binary);
      std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      LLVMFuzzerTestOneInput(buf.data(), buf.size());
      ++n;
    }
  }
  std::printf("replayed %zu inputs\n", n);
  return n > 0 ? 0 : 1;
}
