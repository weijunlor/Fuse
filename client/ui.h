/* client/ui.h
Lobby, login prompt, win screen, cached text. */

#ifndef CLIENT_UI_H
#define CLIENT_UI_H

// Draw text with its top-left corner at (x, y) in pixels, using the current
// GL colour. The text's vertices are regenerated on every call.
void ui_draw_text(float x, float y, float scale, const char *text);

// Width in screen pixels, using the same font and scale as ui_draw_text().
float ui_text_width(float scale, const char *text);

#endif /* CLIENT_UI_H */
