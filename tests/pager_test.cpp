// pager_test.cpp - host-side check of plugin/Sources/pager.hpp (no framework).
// Build + run from the 3ds-guides folder:
//   clang++ -std=c++17 -I plugin/Sources tests/pager_test.cpp -o /tmp/pager_test && /tmp/pager_test

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pager.hpp"

// In-memory "file" that mimics the plugin's File-backed reader:
// reads exactly `length` bytes at `offset`, counts calls, refuses reads past the end
struct MemFile
{
    std::string data;
    int calls = 0;

    uint32_t operator()(uint32_t offset, char *buffer, uint32_t length)
    {
        ++calls;
        assert(offset + length <= data.size()); // pager must never read past EOF
        std::memcpy(buffer, data.data() + offset, length);
        return length;
    }
};

// Build the index of `text` with a given chunk size
static std::vector<uint32_t> Index(const std::string &text, uint32_t chunk = Pager::IndexChunkSize)
{
    MemFile f;
    f.data = text;
    std::vector<uint32_t> index;
    Pager::BuildLineIndex(index, (uint32_t)text.size(), [&f](uint32_t o, char *b, uint32_t l) { return f(o, b, l); }, chunk);
    return index;
}

// Fetch lines [first, first + count) of `text`
static std::vector<std::string> Lines(const std::string &text, uint32_t first, uint32_t count,
                                      uint32_t maxBytes = 66, uint32_t pageChunk = Pager::PageChunkSize)
{
    MemFile f;
    f.data = text;
    std::vector<uint32_t> index = Index(text);
    std::vector<std::string> out;
    Pager::GetLines(out, index, (uint32_t)text.size(), first, count, maxBytes,
                    [&f](uint32_t o, char *b, uint32_t l) { return f(o, b, l); }, pageChunk);
    return out;
}

int main()
{
    // LF input: three lines, trailing newline adds no empty line
    {
        std::string t = "one\ntwo\nthree\n";
        std::vector<uint32_t> idx = Index(t);
        assert((idx == std::vector<uint32_t>{0, 4, 8}));
        std::vector<std::string> l = Lines(t, 0, 10);
        assert((l == std::vector<std::string>{"one", "two", "three"}));
    }

    // CRLF input: same lines, '\r' stripped
    {
        std::string t = "one\r\ntwo\r\nthree\r\n";
        std::vector<uint32_t> idx = Index(t);
        assert((idx == std::vector<uint32_t>{0, 5, 10}));
        std::vector<std::string> l = Lines(t, 0, 10);
        assert((l == std::vector<std::string>{"one", "two", "three"}));
    }

    // Last line without a newline is still a line; empty lines are kept
    {
        assert((Lines("a\nb", 0, 10) == std::vector<std::string>{"a", "b"}));
        assert((Lines("\n\nx", 0, 10) == std::vector<std::string>{"", "", "x"}));
        assert((Lines("a\n", 0, 10) == std::vector<std::string>{"a"}));
        assert(Index("").empty());
        assert(Lines("", 0, 10).empty());
    }

    // Chunk boundaries splitting lines (and splitting "\r|\n"): every chunk size
    // from 1 byte up must give the same index as one big read
    {
        std::string t = "hello world\r\nab\ncd\r\n\r\nlast line no newline";
        std::vector<uint32_t> ref = Index(t);
        assert((ref == std::vector<uint32_t>{0, 13, 16, 20, 22}));
        for (uint32_t chunk = 1; chunk <= t.size() + 3; ++chunk)
            assert(Index(t, chunk) == ref);
        assert((Lines(t, 0, 10) == std::vector<std::string>{"hello world", "ab", "cd", "", "last line no newline"}));
    }

    // Page N extraction: 50 lines, 10 per page
    {
        std::string t;
        char buf[32];
        for (int i = 0; i < 50; ++i)
        {
            std::snprintf(buf, sizeof(buf), "line %d\n", i);
            t += buf;
        }
        const uint32_t visible = 10;
        std::vector<std::string> p2 = Lines(t, 2 * visible, visible);
        assert(p2.size() == 10 && p2.front() == "line 20" && p2.back() == "line 29");
        // A page that runs past the end returns only the remaining lines
        std::vector<std::string> tail = Lines(t, 45, visible);
        assert(tail.size() == 5 && tail.front() == "line 45" && tail.back() == "line 49");
        // Asking past the last line returns nothing
        assert(Lines(t, 50, visible).empty());
    }

    // Clamping the first visible line at the start and at the end
    {
        assert(Pager::ClampTop(-5, 50, 10) == 0);           // before start
        assert(Pager::ClampTop(0, 50, 10) == 0);
        assert(Pager::ClampTop(17, 50, 10) == 17);          // in range
        assert(Pager::ClampTop(40, 50, 10) == 40);          // last full page
        assert(Pager::ClampTop(1000, 50, 10) == 40);        // past the end
        assert(Pager::ClampTop(3, 5, 10) == 0);             // file shorter than a screen
        assert(Pager::ClampTop(3, 0, 10) == 0);             // empty file
        assert(Pager::ClampTop(-10 + 1, 50, 10) == 0);      // L at the top
        assert(Pager::ClampTop(39 + 10, 50, 10) == 40);     // R near the end
    }

    // Long lines are truncated (no wrapping), also when a long line pushes the
    // next lines outside the single page read (per-line fallback read)
    {
        std::string longLine(200, 'x');
        std::string t = longLine + "\r\nshort\n" + longLine + "\nend";
        std::vector<std::string> l = Lines(t, 0, 10, 66, 64); // 64-byte page chunk forces the fallback
        assert(l.size() == 4);
        assert(l[0] == std::string(66, 'x'));
        assert(l[1] == "short");
        assert(l[2] == std::string(66, 'x'));
        assert(l[3] == "end");
        // Exactly maxBytes characters + CRLF: kept whole, ending stripped
        assert((Lines(std::string(66, 'y') + "\r\nz", 0, 2) == std::vector<std::string>{std::string(66, 'y'), "z"}));
    }

    // A normal page is fetched with one read
    {
        MemFile f;
        for (int i = 0; i < 100; ++i)
            f.data += "some guide text line\r\n";
        std::vector<uint32_t> index = Index(f.data);
        std::vector<std::string> out;
        Pager::GetLines(out, index, (uint32_t)f.data.size(), 30, 24, 66,
                        [&f](uint32_t o, char *b, uint32_t l) { return f(o, b, l); });
        assert(out.size() == 24 && f.calls == 1);
    }

    std::printf("pager_test: all checks passed\n");
    return 0;
}
