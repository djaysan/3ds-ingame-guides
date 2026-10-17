#include <3ds.h>
#include "csvc.h"
#include <CTRPluginFramework.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pager.hpp"

namespace CTRPluginFramework
{
    // This function is called before main and before the game starts
    // Useful to do code edits safely
    void    PatchProcess(FwkSettings &settings)
    {
        // B at the menu root closes the menu (default is Select or the touch X only)
        settings.CloseMenuWithB = true;
        // Memory: the plugin block is carved out of the GAME's application memory on
        // old 3DS. 5 MiB is the floor for CTRPF 0.8.0: a 2 MiB build gave
        // "fatal plugin error" at boot (2026-10-16). Bowser's Inside Story DX still
        // crashes at boot with a plugin loaded (memory-tight game) = known limit.
    }

    // This function is called when the process exits
    // Useful to save settings, undo patchs or clean up things
    void    OnProcessExit(void)
    {
    }

    // ---- Guide reader (M2) ----------------------------------------------

    // Guide paths, set per game from the low word of the title ID (M3):
    // /guides/3ds/<TIDLOW>.txt = the guide, /guides/3ds/<TIDLOW>.pos = saved line.
    // In CTRPF paths a leading '/' is the SD card root (FSPath::SdmcFixPath checks
    // for it); no leading '/' = plugin working dir.
    // Plain char arrays, not std::string: nothing of ours runs a destructor at exit.
    static char             GuidePath[32];
    static char             PosPath[32];

    // Screen sizes in pixels
    static const u32        TopWidth = 400;
    static const u32        BottomWidth = 320;
    static const u32        ScreenHeight = 240;

    // Plugin font: Screen::Draw returns posY + 10, so one text line is 10 px high
    static const u32        LineHeight = 10;
    // Left margin of the guide text on the top screen
    static const u32        Margin = 2;

    // Hold-to-repeat for Up/Down: first repeat after 15 frames, then every 3 frames
    static const u32        RepeatDelay = 15;
    static const u32        RepeatRate = 3;

    // Buttons that must be released before the game resumes, so the B (or L/R/Select)
    // press never reaches the game. Circle pad and touch are left out so a drifting
    // stick can't keep us waiting.
    static const u32        ButtonMask = Key::A | Key::B | Key::X | Key::Y | Key::L | Key::R
                                       | Key::Start | Key::Select | Key::DPadUp | Key::DPadDown
                                       | Key::DPadLeft | Key::DPadRight;

    // Running title ID as 16 hex digits (for mapping games to guides later)
    static std::string  TitleIdString(void)
    {
        u64     tid = Process::GetTitleID();
        char    buf[17];

        snprintf(buf, sizeof(buf), "%08lX%08lX", (unsigned long)(tid >> 32), (unsigned long)(tid & 0xFFFFFFFF));
        return buf;
    }

    // Wait (max ~2 s) until every button is up, keeping the reader on screen
    static void     WaitButtonsReleased(void)
    {
        for (int i = 0; i < 120; ++i)
        {
            Controller::Update();
            if ((Controller::GetKeysDown() & ButtonMask) == 0)
                break;
            svcSleepThread(16666667LL); // ~1 frame
        }
    }

    // Top screen: black background, then one plugin-font line per 10 px row.
    // When there is no guide, `message` is shown instead of the lines.
    static void     DrawTop(const std::vector<std::string> &lines, const std::string &message)
    {
        const Screen &top = OSD::GetTopScreen();

        top.DrawRect(0, 0, TopWidth, ScreenHeight, Color::Black, true);

        if (!message.empty())
        {
            top.Draw(message, Margin, 0, Color::White, Color::Black);
            return;
        }

        u32 y = 0;
        for (const std::string &line : lines)
            y = top.Draw(line, Margin, y, Color::White, Color::Black);
    }

    // Bottom screen: status bar with file name, position, title ID and key legend
    static void     DrawBottom(const std::string &position, const std::string &titleId, bool hasGuide)
    {
        const Screen &bottom = OSD::GetBottomScreen();

        bottom.DrawRect(0, 0, BottomWidth, ScreenHeight, Color::Black, true);

        u32 y = 4;
        y = bottom.Draw("3DS Guides", 4, y, Color::Orange, Color::Black);
        y += 6;
        y = bottom.Draw(std::string("File: ") + GuidePath, 4, y, Color::White, Color::Black);
        y = bottom.Draw(position, 4, y, Color::White, Color::Black);
        y = bottom.Draw("Title ID: " + titleId, 4, y, Color::White, Color::Black);

        // Key legend pinned to the bottom edge, above a thin separator
        bottom.DrawRect(0, ScreenHeight - 18, BottomWidth, 1, Color::Gray, true);
        bottom.Draw(hasGuide ? "L/R page  Up/Dn line  B close" : "B close", 4, ScreenHeight - 14,
                    Color::Yellow, Color::Black);
    }

    // The reader: pauses the game (unless the menu already did), shows the guide
    // until B, then resumes the game. Runs on the plugin thread, like MessageBox.
    static void     RunGuideReader(void)
    {
        // Pause the game ourselves on the hotkey path; the menu path is already paused.
        // Process::Pause() blocks until the game is frozen and hands us the screens.
        bool    pausedHere = false;
        if (!Process::IsPaused())
        {
            Process::Pause();
            pausedHere = true;
        }

        const std::string titleId = TitleIdString();

        // Guide file named after the title ID low word, e.g. 000EC400 = ALBW EUR
        snprintf(GuidePath, sizeof(GuidePath), "/guides/3ds/%s.txt", titleId.c_str() + 8);
        snprintf(PosPath, sizeof(PosPath), "/guides/3ds/%s.pos", titleId.c_str() + 8);

        // Plugin font is monospace: measure one glyph (fallback 6 px) to get the columns
        u32     charWidth = (u32)OSD::GetTextWidth(false, "W");
        if (charWidth == 0)
            charWidth = 6;
        const u32   maxCols = (TopWidth - Margin) / charWidth;     // ~66
        const u32   visible = ScreenHeight / LineHeight;           // 24

        // Show something right away: indexing a big guide takes a moment on SD
        {
            const Screen &top = OSD::GetTopScreen();
            top.DrawRect(0, 0, TopWidth, ScreenHeight, Color::Black, true);
            top.Draw("Loading guide...", Margin, 0, Color::White, Color::Black);
            OSD::SwapBuffers();
        }

        // Open the guide and build the line-offset index once. The file stays open
        // for the whole session; pages are read from it on demand.
        File                file;
        std::vector<u32>    index;
        u32                 fileSize = 0;
        bool                hasGuide = false;
        std::string         message;

        if (File::Exists(GuidePath) == 1 && File::Open(file, GuidePath, File::READ) == File::SUCCESS)
        {
            u64 size = file.GetSize();
            fileSize = (size > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (u32)size;
            hasGuide = true;
        }
        else
            message = std::string("No guide at ") + GuidePath;

        // Byte reader for the pager: seek + read exactly `length` bytes (File::Read
        // returns 0 on success, not a byte count; the pager never reads past EOF)
        auto reader = [&file](u32 offset, char *buffer, u32 length) -> u32
        {
            if (file.Seek(offset, File::SET) != File::SUCCESS)
                return 0;
            if (file.Read(buffer, length) != File::SUCCESS)
                return 0;
            return length;
        };

        if (hasGuide)
        {
            Pager::BuildLineIndex(index, fileSize, reader);
            if (index.empty())
                message = std::string("Guide is empty: ") + GuidePath;
        }

        const u32   lineCount = (u32)index.size();
        u32         top = 0;            // first visible line

        // Saved reading position: a decimal line number in the .pos file, clamped
        u32         savedTop = 0;
        if (lineCount > 0 && File::Exists(PosPath) == 1)
        {
            File    pf;
            char    buf[16] = {0};
            if (File::Open(pf, PosPath, File::READ) == File::SUCCESS)
            {
                u64 n = pf.GetSize();
                if (n > 0 && n < sizeof(buf) && pf.Read(buf, (u32)n) == File::SUCCESS)
                    savedTop = (u32)strtoul(buf, nullptr, 10);
                pf.Close();
            }
            top = Pager::ClampTop(savedTop, lineCount, visible);
            savedTop = top;
        }
        bool        reload = true;      // page text must be re-read from the file
        int         redraw = 2;         // frames left to draw (2 = both buffers of the double buffer)
        u32         heldFrames = 0;     // how long Up/Down has been held, for repeat
        std::vector<std::string>    lines;

        while (true)
        {
            Controller::Update();

            // B closes the reader
            if (Controller::IsKeyPressed(Key::B))
                break;

            if (lineCount > 0)
            {
                int64_t wanted = top;

                // Up/Down: one line, on the first frame then with hold-to-repeat
                u32 keys = Controller::GetKeysDown();
                int dir = (keys & Key::DPadDown) ? 1 : ((keys & Key::DPadUp) ? -1 : 0);
                if (dir == 0)
                    heldFrames = 0;
                else
                {
                    if (heldFrames == 0
                        || (heldFrames >= RepeatDelay && (heldFrames - RepeatDelay) % RepeatRate == 0))
                        wanted += dir;
                    ++heldFrames;
                }

                // L/R: one screen of lines back/forward
                if (Controller::IsKeyPressed(Key::L))
                    wanted -= visible;
                if (Controller::IsKeyPressed(Key::R))
                    wanted += visible;

                // Keep the first visible line inside the file (last page stays full)
                u32 newTop = Pager::ClampTop(wanted, lineCount, visible);
                if (newTop != top)
                {
                    top = newTop;
                    reload = true;
                }
            }

            // Re-read only the visible lines from the SD card when the position changed
            if (reload)
            {
                if (lineCount > 0)
                    Pager::GetLines(lines, index, fileSize, top, visible, maxCols, reader);
                reload = false;
                redraw = 2;
            }

            // Draw only after a change (twice, once per buffer), then present the frame.
            // SwapBuffers waits for the GPU swap, which paces this loop at the screen rate.
            if (redraw > 0)
            {
                char position[48];
                snprintf(position, sizeof(position), "Line %lu / %lu",
                         (unsigned long)(lineCount ? top + 1 : 0), (unsigned long)lineCount);
                DrawTop(lines, message);
                DrawBottom(position, titleId, lineCount > 0);
                --redraw;
            }
            OSD::SwapBuffers();
        }

        if (file.IsOpen())
            file.Close();

        // Remember the line we stopped at. Written only when it changed, so a
        // plain open/close costs no SD write.
        if (hasGuide && lineCount > 0 && top != savedTop)
        {
            File    pf;
            if (File::Open(pf, PosPath, File::RWC | File::TRUNCATE) == File::SUCCESS)
            {
                char    buf[16];
                int     len = snprintf(buf, sizeof(buf), "%lu", (unsigned long)top);
                pf.Write(buf, (u32)len);
                pf.Close();
            }
        }

        // Don't let the closing B press (or anything still held) leak into the game
        WaitButtonsReleased();

        // Resume the game exactly where it was (only if we were the one pausing it)
        if (pausedHere)
            Process::Play();
    }

    // Menu hotkey gate. CTRPF calls this when the menu hotkey (Select) fires, BEFORE
    // it pauses the game; returning false cancels the opening (no pause, no menu).
    static bool     OnMenuOpening(void)
    {
        const u32 combo = Key::L | Key::R;

        // Plain Select: open nothing, the game keeps its Select press
        if ((Controller::GetKeysDown() & combo) != combo)
            return false;

        // L+R+Select: show the guide right here and never show the menu list
        RunGuideReader();
        return false;
    }

    // Menu callback for the "Guide" entry: same reader, second way in
    static void    GuideMenu(MenuEntry *entry)
    {
        RunGuideReader();
    }

    void    InitMenu(PluginMenu &menu)
    {
        // One entry: no per-frame game function (nullptr), only a menu callback
        // ponytail: unreachable (ForceOpen goes through the OnOpening gate too); kept so the menu has one entry
        menu += new MenuEntry("Guide", nullptr, GuideMenu, "Open the text guide from the SD card.");
    }

    int     main(void)
    {
        PluginMenu *menu = new PluginMenu("3DS Guides", 0, 1, 0,
                                            "In-game guides for 3DS games.");

        // Synnchronize the menu with frame event
        menu->SynchronizeWithFrame(true);

        // Only L+R+Select opens anything (the guide reader); plain Select stays with the game
        menu->OnOpening = OnMenuOpening;

        // Init our menu entries & folders
        InitMenu(*menu);

        // Launch menu and mainloop
        menu->Run();

        // ponytail: no `delete menu` here. Run() only returns when the game process is
        // exiting, and in libctrpf 0.8.0 PluginMenu's destructor double-frees its items
        // (MenuItem::~MenuItem -> free() data abort, Luma dumps 2026-10-16). The OS
        // reclaims the plugin memory anyway.

        // Exit plugin
        return (0);
    }
}
