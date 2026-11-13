#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
constexpr std::size_t BUFFER_SIZE = 64 * 1024;

// Вычисляет 64-битный хэш FNV-1a для содержимого файла.
std::uint64_t calculate_hash(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("не удалось открыть файл '" + path.string() + "'");
    }

    std::uint64_t hash = FNV_OFFSET_BASIS;
    std::array<char, BUFFER_SIZE> buffer{};

    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes_read = input.gcount();

        for (std::streamsize index = 0; index < bytes_read; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= FNV_PRIME;
        }
    }

    if (input.bad()) {
        throw std::runtime_error("ошибка чтения файла '" + path.string() + "'");
    }

    return hash;
}

// Рекурсивно собирает обычные файлы. Символические ссылки пропускаются.
std::vector<fs::path> collect_files(const fs::path& root) {
    std::vector<fs::path> files;

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (fs::is_regular_file(entry.symlink_status())) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

// Безопасно заменяет дубликат жёсткой ссылкой на оставленный файл.
void replace_with_hard_link(const fs::path& original,
                            const fs::path& duplicate) {
    static unsigned long counter = 0;
    ++counter;

    const fs::path temporary = duplicate.parent_path() /
        (".hardlink_tmp_" + std::to_string(getpid()) + "_" +
         std::to_string(counter));

    // Сначала создаётся готовая ссылка. Исходный дубликат пока не удаляется.
    fs::create_hard_link(original, temporary);

    try {
        // В Linux rename атомарно заменяет дубликат готовой жёсткой ссылкой.
        fs::rename(temporary, duplicate);
    } catch (...) {
        std::error_code ignored_error;
        fs::remove(temporary, ignored_error);
        throw;
    }
}

void print_usage(const char* program_name) {
    std::cerr << "Использование: " << program_name << " <каталог>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        const fs::path root = fs::absolute(argv[1]);

        if (!fs::is_directory(root)) {
            std::cerr << "Ошибка: указанный каталог не существует.\n";
            return 2;
        }

        const std::vector<fs::path> files = collect_files(root);

        std::cout << "Найдено обычных файлов: " << files.size() << '\n';
        for (const fs::path& file : files) {
            std::cout << "  " << file.string() << '\n';
        }

        // Для каждого хэша хранится путь к первому найденному файлу.
        std::map<std::uint64_t, fs::path> originals;
        std::size_t replaced_count = 0;

        std::cout << "\nХэши файлов:\n";
        for (const fs::path& file : files) {
            const std::uint64_t hash = calculate_hash(file);
            std::cout << "  " << std::hex << hash << std::dec
                      << "  " << file.string() << '\n';

            const auto [iterator, inserted] = originals.emplace(hash, file);
            if (inserted) {
                continue;
            }

            const fs::path& original = iterator->second;
            if (fs::equivalent(original, file)) {
                std::cout << "    Уже является жёсткой ссылкой на: "
                          << original.string() << '\n';
                continue;
            }

            replace_with_hard_link(original, file);
            ++replaced_count;
            std::cout << "    Заменён жёсткой ссылкой на: "
                      << original.string() << '\n';
        }

        std::cout << "\nГотово. Заменено файлов: "
                  << replaced_count << ".\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << '\n';
        return 3;
    }
}
