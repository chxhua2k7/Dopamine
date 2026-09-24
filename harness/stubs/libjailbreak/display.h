#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
typedef double CGFloat;
typedef struct { CGFloat width, height; } CGSize;
struct drawctx { bool inited; void *base; CGSize size; int bytesPerRow; void *framebuffer; void *surface; int lastSwapToken; };
struct drawctx *drawctx_init(void);
void drawctx_free(struct drawctx *ctx);
int drawctx_draw_raw(struct drawctx *ctx, void *rawBuf, size_t rawBufSize);
CGFloat get_main_screen_rotation(void);
#endif
