#ifndef GW_SOUND_H
#define GW_SOUND_H

void snd_init(void);
void snd_shutdown(void);
void snd_reset(void);
void snd_beep(void);
void snd_tone(int freq_hz, int duration_ticks);
void snd_tone_sync(int freq_hz, int duration_ticks);
void snd_play(const char *mml);
void snd_start_continuous(int freq_hz);
void snd_stop_continuous(void);

#endif
