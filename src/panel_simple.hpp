// MGPU Bridge - panel_simple: the player-facing view of the panel. See panel_simple.cpp for what it
// is and what it is not (it drives the same setters as the full panel; the layout is data, not code).
#pragma once

#include <windows.h>

namespace mgpu::psimple
{
    // Called once from DllMain: remembers the add-on's directory, where panel_simple.txt lives.
    // R330 (F2): no file read and no log line here.
    void init(HMODULE mod);

    bool enabled();                  // PanelSimple=1 AND the layout file has at least one row
    void set_enabled(bool on);       // writes PanelSimple to mgpu.ini
    void toggle();                   // R330 (F1): flips PanelSimple (own flag) and writes it; any thread, no layout access
    bool cjk();                      // the panel font ReShade loaded carries CJK glyphs

    void reload();                   // re-read panel_simple.txt now
    int  layout_count();
    void open_editor();              // panel_simple.txt in notepad
    void export_items();             // every registered row -> panel_simple_items.txt

    typedef void (*draw_fn)();

    // Register one controllable row. Call once per id, from draw_simple_panel's table.
    void row(const char *id, const char *en, draw_fn fn);

    // Draws the row's label (Chinese when the font allows, English otherwise) and the help marker.
    void label(const char *id, const char *en);
    void help(const char *id);
    const char *zh(const char *id);  // nullptr when the file has no Chinese label for id

    void draw_simple_panel();        // the mode itself: rows in the file's order + Advanced button
    void draw_editor();              // the full panel's section: toggle, open, export, reload
}
