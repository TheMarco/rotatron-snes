#ifndef AUDIO_H
#define AUDIO_H

void audioInit(void);  /* boot SPC700 + load and start the BGM loop */
void audioFrame(void); /* call once per frame */

#endif
