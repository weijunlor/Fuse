/** @file client/ui.c
 * @brief Shared text drawing plus width measurement for centred labels.
 */

#include <GLFW/glfw3.h>

#include "ui.h"
#include "stb_easy_font.h"

float ui_text_width(float scale, const char *text)
{
    return stb_easy_font_width((char *)text) * scale;
}

void ui_draw_text(float x, float y, float scale, const char *text)
{
    // Increased buffer size to support longer strings
    char buffer[16384];

    int num_quads = stb_easy_font_print(
        0, 0,
        (char *)text,
        NULL,
        buffer,
        sizeof(buffer)
    );

    glPushMatrix();

    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);

    glEnableClientState(GL_VERTEX_ARRAY);

    glVertexPointer(2, GL_FLOAT, 16, buffer);
    glDrawArrays(GL_QUADS, 0, num_quads * 4);

    glDisableClientState(GL_VERTEX_ARRAY);

    glPopMatrix();
}
