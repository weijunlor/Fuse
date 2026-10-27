/* client/render.h
Batched rendering of players, bomb and HUD. */

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

// Set up a pixel-coordinate projection with (0, 0) at the top-left corner
void render_init(void);

// Draw one frame: the banner and every active player's square and ID
void render_frame(void);

#endif /* CLIENT_RENDER_H */
