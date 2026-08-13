/* movie_player.h -- native FMV playback for local.mediav.MoviePlayer
 *
 * Chaos Rings 3 streams its FMVs out of CRDBmov.android.mvgl through a Java
 * MoviePlayer; we decode them natively instead (see movie_player.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __MOVIE_PLAYER_H__
#define __MOVIE_PLAYER_H__

// resolve the engine's Fios reader at boot (before its symbol table is freed)
void movie_player_init(void);
void movie_set_db(const char *db_path);
int  movie_play(const char *name, int looping); // returns 1 if a movie played
void movie_stop(void);
void movie_pause(void);
void movie_resume(void);
int  movie_is_playing(void);

#endif
