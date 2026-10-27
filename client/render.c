/* client/render.c
Batched rendering of players, bomb and HUD. */

#include <stdio.h>
#include <GLFW/glfw3.h>

#include "render.h"
#include "net_client.h"
#include "ui.h"

void render_init(void)
{
    /* Use pixel coordinates */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);
}

void render_frame(void)
{
	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1.0f, 0.0f, 0.0f);

	char str_banner[100] = "Waiting for server ...";
	if (int_player_id != -1)
	{
		snprintf(str_banner, sizeof(str_banner), "Player %d", int_player_id);
	}
	ui_draw_text(320, 20, 3.0f, str_banner);

	for(int i = 0; i < MAX_PLAYERS; i++)
	{
		pthread_mutex_lock(&mutex);
		if(arr_Players[i].b_active == true)
		{
			// Label the player ID
			char str_player[2] = "";
			snprintf(str_player, sizeof(str_player), "%d", i+1);

			ui_draw_text(arr_Players[i].x, arr_Players[i].y, 3.0f, str_player);

			// Square outline
			glBegin(GL_LINE_LOOP);
				glVertex2f(arr_Players[i].x, arr_Players[i].y);
				glVertex2f(arr_Players[i].x, arr_Players[i].y + SQ_WIDTH);
				glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y + SQ_WIDTH);
				glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y);
			glEnd();
		}
		pthread_mutex_unlock(&mutex);
	}
}
