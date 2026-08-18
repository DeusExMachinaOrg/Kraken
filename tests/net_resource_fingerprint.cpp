#include "net/resource_fingerprint.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kraken::net::ResourceFingerprintErrorCode;
using kraken::net::ResourceFingerprintPolicy;
using kraken::net::ResourceFingerprintRequest;
using kraken::net::ResourceFingerprintResult;

int failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

struct TemporaryTree {
    fs::path path;

    ~TemporaryTree()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

TemporaryTree make_tree(const char* suffix)
{
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    TemporaryTree tree{fs::temp_directory_path() /
                       (std::string("kraken_resource_fingerprint_") + suffix +
                        "_" + std::to_string(stamp))};
    fs::create_directories(tree.path / "Data");
    fs::create_directories(tree.path / "Nested" / "More");
    fs::create_directories(tree.path / "build");
    fs::create_directories(tree.path / "Nested" / "cache");
    fs::create_directories(tree.path / "log");
    fs::create_directories(tree.path / "save");
    fs::create_directories(tree.path / "exception");
    return tree;
}

void write_file(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output)
        throw std::runtime_error("test file write failed");
}

ResourceFingerprintPolicy policy()
{
    return {{"build", "cache", "log", "save", "exception"}};
}

ResourceFingerprintResult fingerprint(const fs::path& root)
{
    return kraken::net::fingerprint_resources(
        ResourceFingerprintRequest{root, {fs::path{"."}}, policy()});
}

bool printable_hex_digest(const std::string& digest)
{
    constexpr std::string_view prefix =
        kraken::net::kResourceFingerprintPrefix;
    if (!digest.starts_with(prefix) || digest.size() != prefix.size() + 64)
        return false;
    return std::all_of(digest.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       digest.end(), [](const char character) {
                           return std::isxdigit(
                               static_cast<unsigned char>(character)) != 0;
                       });
}

void populate_tree(const fs::path& root)
{
    write_file(root / "Data" / "Alpha.BIN", std::string("alpha\0bytes", 11));
    write_file(root / "Nested" / "More" / "beta.txt", "second file");
    write_file(root / "build" / "ignored.bin", "ignored build");
    write_file(root / "Nested" / "cache" / "ignored.bin", "ignored cache");
    write_file(root / "log" / "ignored.log", "ignored log");
    write_file(root / "save" / "ignored.save", "ignored save");
    write_file(root / "exception" / "ignored.dmp", "ignored exception");
}

void test_identical_trees_and_policy()
{
    TemporaryTree first = make_tree("first");
    TemporaryTree second = make_tree("second");
    populate_tree(first.path);
    populate_tree(second.path);

    const ResourceFingerprintResult first_result = fingerprint(first.path);
    const ResourceFingerprintResult second_result = fingerprint(second.path);
    CHECK(first_result.succeeded());
    CHECK(second_result.succeeded());
    CHECK(first_result.digest == second_result.digest);
    CHECK(first_result.stats.file_count == 2);
    CHECK(first_result.stats.total_bytes == 22);
    CHECK(first_result.stats.ignored_directory_count == 5);
    CHECK(printable_hex_digest(first_result.digest));

    write_file(first.path / "build" / "ignored.bin", "changed ignored bytes");
    write_file(first.path / "Nested" / "cache" / "new.cache", "ignored");
    const ResourceFingerprintResult ignored_change = fingerprint(first.path);
    CHECK(ignored_change.succeeded());
    CHECK(ignored_change.digest == second_result.digest);

    write_file(first.path / "Nested" / "More" / "beta.txt", "changed content");
    const ResourceFingerprintResult content_change = fingerprint(first.path);
    CHECK(content_change.succeeded());
    CHECK(content_change.digest != second_result.digest);
}

void test_order_case_and_path_changes()
{
    TemporaryTree first = make_tree("order-first");
    TemporaryTree second = make_tree("order-second");
    populate_tree(first.path);
    write_file(second.path / "data" / "alpha.bin",
               std::string("alpha\0bytes", 11));
    write_file(second.path / "nested" / "more" / "BETA.TXT", "second file");

    const ResourceFingerprintResult whole_first = fingerprint(first.path);
    const ResourceFingerprintResult whole_second = fingerprint(second.path);
    CHECK(whole_first.succeeded());
    CHECK(whole_second.succeeded());
    CHECK(whole_first.digest == whole_second.digest);

    const ResourceFingerprintResult reordered =
        kraken::net::fingerprint_resources(ResourceFingerprintRequest{
            first.path, {"Nested", "Data"}, policy()});
    CHECK(reordered.succeeded());
    CHECK(reordered.digest == whole_first.digest);

    fs::rename(second.path / "nested" / "more" / "BETA.TXT",
               second.path / "nested" / "more" / "gamma.txt");
    const ResourceFingerprintResult path_change = fingerprint(second.path);
    CHECK(path_change.succeeded());
    CHECK(path_change.digest != whole_first.digest);
}

void test_rejected_inputs()
{
    TemporaryTree tree = make_tree("errors");
    populate_tree(tree.path);

    const ResourceFingerprintResult missing =
        kraken::net::fingerprint_resources(ResourceFingerprintRequest{
            tree.path, {"does-not-exist"}, policy()});
    CHECK(!missing.succeeded());
    CHECK(missing.error == ResourceFingerprintErrorCode::MissingInput);

    const ResourceFingerprintResult duplicate =
        kraken::net::fingerprint_resources(ResourceFingerprintRequest{
            tree.path, {".", "Data/Alpha.BIN"}, policy()});
    CHECK(!duplicate.succeeded());
    CHECK(duplicate.error == ResourceFingerprintErrorCode::DuplicateRelativePath);

    const ResourceFingerprintResult outside =
        kraken::net::fingerprint_resources(ResourceFingerprintRequest{
            tree.path, {"../outside"}, policy()});
    CHECK(!outside.succeeded());
    CHECK(outside.error == ResourceFingerprintErrorCode::InputOutsideInstallRoot);

    const ResourceFingerprintResult invalid_policy =
        kraken::net::fingerprint_resources(
            ResourceFingerprintRequest{tree.path, {"."}, {{"../cache"}}});
    CHECK(!invalid_policy.succeeded());
    CHECK(invalid_policy.error ==
          ResourceFingerprintErrorCode::InvalidIgnorePolicy);
}

} // namespace

int main()
{
    try {
        test_identical_trees_and_policy();
        test_order_case_and_path_changes();
        test_rejected_inputs();
    } catch (const std::exception& exception) {
        std::cerr << "resource fingerprint test threw: " << exception.what()
                  << '\n';
        return 2;
    }
    if (failures != 0)
        return 1;
    std::cout << "resource fingerprint tests passed\n";
    return 0;
}
