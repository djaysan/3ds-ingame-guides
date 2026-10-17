// pager.hpp - line index + page extraction for the guide reader.
//
// Pure C++11, no CTRPF / libctru dependency, so the same code builds in the
// plugin (gnu++11) and in the host unit test (tests/pager_test.cpp).
//
// The file itself is never loaded whole: callers pass a "reader" callable
//     uint32_t read(uint32_t offset, char *buffer, uint32_t length)
// that reads exactly `length` bytes at `offset` and returns how many bytes it
// read (0 on error). The pager never asks for bytes past `fileSize`.

#ifndef PAGER_HPP
#define PAGER_HPP

#include <stdint.h>
#include <string>
#include <vector>

namespace Pager
{
    // Bytes read per call while scanning the file for line starts
    static const uint32_t IndexChunkSize = 8192;

    // Upper bound for the single read that fetches a whole visible page
    static const uint32_t PageChunkSize = 8192;

    // Build the line-start index: index[i] = byte offset where line i starts.
    // - Line 0 starts at offset 0 (an empty file has no lines).
    // - Each '\n' ends a line; the next line starts right after it.
    // - A trailing '\n' does not create an extra empty line.
    // - A last line without '\n' is still a line.
    // - "\r\n" needs no special case here: the '\r' stays at the end of its
    //   line and is stripped when the line is extracted (TrimLine).
    template <typename Reader>
    void    BuildLineIndex(std::vector<uint32_t> &index, uint32_t fileSize,
                           Reader read, uint32_t chunkSize = IndexChunkSize)
    {
        index.clear();
        if (fileSize == 0 || chunkSize == 0)
            return;

        // Line 0 always starts at the top of the file
        index.push_back(0);

        std::vector<char> chunk(chunkSize);
        uint32_t offset = 0;

        // Walk the file chunk by chunk; a line split across two chunks needs
        // no care because only '\n' positions matter, never line contents
        while (offset < fileSize)
        {
            uint32_t length = fileSize - offset;
            if (length > chunkSize)
                length = chunkSize;

            uint32_t got = read(offset, &chunk[0], length);
            if (got == 0)
                break; // read error: keep the lines found so far
            if (got > length)
                got = length;

            for (uint32_t i = 0; i < got; ++i)
            {
                if (chunk[i] == '\n')
                {
                    uint32_t next = offset + i + 1;
                    // Only record a new line if something follows the '\n'
                    if (next < fileSize)
                        index.push_back(next);
                }
            }
            offset += got;
        }
    }

    // Offset just past line `line` (its newline included): the start of the
    // next line, or the end of the file for the last line
    inline uint32_t LineEnd(const std::vector<uint32_t> &index, uint32_t fileSize, uint32_t line)
    {
        return (line + 1 < index.size()) ? index[line + 1] : fileSize;
    }

    // Highest valid first-visible line so the last page is full
    // (0 when the whole file fits on one screen)
    inline uint32_t MaxTop(uint32_t lineCount, uint32_t visible)
    {
        return (lineCount > visible) ? lineCount - visible : 0;
    }

    // Clamp a wanted first-visible line (may be negative or past the end)
    // into [0, MaxTop]
    inline uint32_t ClampTop(int64_t wanted, uint32_t lineCount, uint32_t visible)
    {
        if (wanted < 0)
            return 0;
        uint32_t maxTop = MaxTop(lineCount, visible);
        if (wanted > (int64_t)maxTop)
            return maxTop;
        return (uint32_t)wanted;
    }

    // Remove the line ending ("\n" or "\r\n") and cut the line to maxBytes.
    // No word wrapping: guides are pre-wrapped on the Mac side.
    inline void TrimLine(std::string &text, uint32_t maxBytes)
    {
        while (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == '\r'))
            text.erase(text.size() - 1);
        // Expand tabs to the next 8-column stop: the plugin font has no tab glyph and
        // about a quarter of the GameFAQs guides keep tabs on short lines
        if (text.find('\t') != std::string::npos)
        {
            std::string expanded;
            expanded.reserve(text.size() + 16);
            for (char c : text)
            {
                if (c == '\t')
                    expanded.append(8 - (expanded.size() % 8), ' ');
                else
                    expanded.push_back(c);
            }
            text.swap(expanded);
        }
        if (text.size() > maxBytes)
            text.resize(maxBytes);
    }

    // Fetch up to `count` lines starting at line `first`, each trimmed to
    // maxBytes, into `out`. Lines past the end are simply not returned.
    // One read covers the whole page (capped at chunkSize); a line that falls
    // outside that read (only possible after a very long line) is read alone.
    template <typename Reader>
    void    GetLines(std::vector<std::string> &out, const std::vector<uint32_t> &index,
                     uint32_t fileSize, uint32_t first, uint32_t count, uint32_t maxBytes,
                     Reader read, uint32_t chunkSize = PageChunkSize)
    {
        out.clear();
        const uint32_t total = (uint32_t)index.size();
        if (first >= total || count == 0)
            return;

        // Last line (exclusive) of the page, clamped to the file
        uint64_t lastWide = (uint64_t)first + count;
        uint32_t last = (lastWide > total) ? total : (uint32_t)lastWide;

        // One read from the first visible line, as long as the page or the cap
        const uint32_t start = index[first];
        const uint32_t span = LineEnd(index, fileSize, last - 1) - start;
        const uint32_t bufLen = (span < chunkSize) ? span : chunkSize;

        std::vector<char> buf(bufLen ? bufLen : 1);
        uint32_t got = bufLen ? read(start, &buf[0], bufLen) : 0;
        if (got > bufLen)
            got = bufLen;

        for (uint32_t line = first; line < last; ++line)
        {
            const uint32_t begin = index[line];
            const uint32_t length = LineEnd(index, fileSize, line) - begin;

            // Only maxBytes characters are shown; +2 keeps a "\r\n" visible to TrimLine
            const uint32_t want = (length < maxBytes + 2) ? length : maxBytes + 2;
            const uint32_t rel = begin - start;

            std::string text;
            if (rel + want <= got)
            {
                // Line is inside the page chunk
                text.assign(&buf[rel], want);
            }
            else if (want > 0)
            {
                // Line lies past the chunk: read just the visible part of it
                std::vector<char> one(want);
                uint32_t n = read(begin, &one[0], want);
                if (n > want)
                    n = want;
                if (n > 0)
                    text.assign(&one[0], n);
            }

            TrimLine(text, maxBytes);
            out.push_back(text);
        }
    }
}

#endif // PAGER_HPP
