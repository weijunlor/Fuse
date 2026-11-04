/** @file client/ui.h
 * @brief Shared font drawing and width measurement for centred game labels.
 */

#ifndef CLIENT_UI_H
#define CLIENT_UI_H

// Draw text with its top-left corner at (x, y) in pixels, using the current
// GL colour. The text's vertices are regenerated on every call.
/** @brief Draw untextured text using the current OpenGL colour.
 * @param x Left coordinate in logical pixels.
 * @param y Top coordinate in logical pixels.
 * @param scale Font scale factor.
 * @param text Null-terminated text to draw.
 * @pre A current GL context with the game's pixel-coordinate projection.
 */
void ui_draw_text(float x, float y, float scale, const char *text);

// Width in screen pixels, using the same font and scale as ui_draw_text().
/** @brief Measure text for centring banners and player/health labels.
 * @param scale Font scale factor, matching ui_draw_text().
 * @param text Null-terminated text to measure.
 * @return Width in logical pixels at the requested scale.
 */
float ui_text_width(float scale, const char *text);

#endif /* CLIENT_UI_H */
