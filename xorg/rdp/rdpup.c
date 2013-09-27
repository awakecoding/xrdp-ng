/*
Copyright 2005-2013 Jay Sorg

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#include "rdp.h"

#include <xrdp-ng/xrdp.h>

#include <winpr/crt.h>
#include <winpr/pipe.h>

#define LOG_LEVEL 1
#define LLOG(_level, _args) \
		do { if (_level < LOG_LEVEL) { ErrorF _args ; } } while (0)
#define LLOGLN(_level, _args) \
		do { if (_level < LOG_LEVEL) { ErrorF _args ; ErrorF("\n"); } } while (0)

rdsService* service = NULL;
static int g_clientfd = -1;

static int g_listen_sck = 0;
static int g_sck = 0;
static int g_sck_closed = 0;
static int g_connected = 0;

static int g_begin = 0;
static wStream* g_out_s = 0;
static wStream* g_in_s = 0;
static int g_button_mask = 0;
static int g_count = 0;

static BYTE* pfbBackBufferMemory = NULL;

extern ScreenPtr g_pScreen;
extern int g_Bpp;
extern int g_Bpp_mask;
extern rdpScreenInfoRec g_rdpScreen;
extern int g_use_rail;
extern char g_uds_data[];
extern int g_con_number;

static int g_pixmap_byte_total = 0;
static int g_pixmap_num_used = 0;

/*
0 GXclear,        0
1 GXnor,          DPon
2 GXandInverted,  DPna
3 GXcopyInverted, Pn
4 GXandReverse,   PDna
5 GXinvert,       Dn
6 GXxor,          DPx
7 GXnand,         DPan
8 GXand,          DPa
9 GXequiv,        DPxn
a GXnoop,         D
b GXorInverted,   DPno
c GXcopy,         P
d GXorReverse,   PDno
e GXor,          DPo
f GXset          1
 */

/**
 * DstBlt:
 *
 * GXclear	BLACKNESS 	0x00000042
 * GXnoop	D		0x00AA0029
 * GXinvert	DSTINVERT	0x00550009
 * GXset	WHITENESS	0x00FF0062
 */

static int g_rdp_opcodes[16] =
{
		0x00, /* GXclear        0x0 0 */
		0x88, /* GXand          0x1 src AND dst */
		0x44, /* GXandReverse   0x2 src AND NOT dst */
		0xcc, /* GXcopy         0x3 src */
		0x22, /* GXandInverted  0x4 NOT src AND dst */
		0xaa, /* GXnoop         0x5 dst */
		0x66, /* GXxor          0x6 src XOR dst */
		0xee, /* GXor           0x7 src OR dst */
		0x11, /* GXnor          0x8 NOT src AND NOT dst */
		0x99, /* GXequiv        0x9 NOT src XOR dst */
		0x55, /* GXinvert       0xa NOT dst */
		0xdd, /* GXorReverse    0xb src OR NOT dst */
		0x33, /* GXcopyInverted 0xc NOT src */
		0xbb, /* GXorInverted   0xd NOT src OR dst */
		0x77, /* GXnand         0xe NOT src OR NOT dst */
		0xff  /* GXset          0xf 1 */
};

#define COLOR8(r, g, b) \
		((((r) >> 5) << 0)  | (((g) >> 5) << 3) | (((b) >> 6) << 6))
#define COLOR15(r, g, b) \
		((((r) >> 3) << 10) | (((g) >> 3) << 5) | (((b) >> 3) << 0))
#define COLOR16(r, g, b) \
		((((r) >> 3) << 11) | (((g) >> 2) << 5) | (((b) >> 3) << 0))
#define COLOR24(r, g, b) \
		((((r) >> 0) << 0)  | (((g) >> 0) << 8) | (((b) >> 0) << 16))
#define SPLITCOLOR32(r, g, b, c) \
		{ \
	r = ((c) >> 16) & 0xff; \
	g = ((c) >> 8) & 0xff; \
	b = (c) & 0xff; \
		}

int convert_pixel(int in_pixel)
{
	int red;
	int green;
	int blue;
	int rv;

	rv = 0;

	if (g_rdpScreen.depth == 24)
	{
		if (g_rdpScreen.rdp_bpp == 32)
		{
			rv = in_pixel;
			SPLITCOLOR32(red, green, blue, rv);
			rv = COLOR24(red, green, blue);
		}
		else if (g_rdpScreen.rdp_bpp == 24)
		{
			rv = in_pixel;
			SPLITCOLOR32(red, green, blue, rv);
			rv = COLOR24(red, green, blue);
		}
		else if (g_rdpScreen.rdp_bpp == 16)
		{
			rv = in_pixel;
			SPLITCOLOR32(red, green, blue, rv);
			rv = COLOR16(red, green, blue);
		}
		else if (g_rdpScreen.rdp_bpp == 15)
		{
			rv = in_pixel;
			SPLITCOLOR32(red, green, blue, rv);
			rv = COLOR15(red, green, blue);
		}
		else if (g_rdpScreen.rdp_bpp == 8)
		{
			rv = in_pixel;
			SPLITCOLOR32(red, green, blue, rv);
			rv = COLOR8(red, green, blue);
		}
	}
	else if (g_rdpScreen.depth == g_rdpScreen.rdp_bpp)
	{
		return in_pixel;
	}

	return rv;
}

static int rdpup_disconnect(void)
{
	RemoveEnabledDevice(g_sck);
	g_connected = 0;
	g_tcp_close(g_sck);
	g_sck = 0;
	g_sck_closed = 1;
	g_pixmap_byte_total = 0;
	g_pixmap_num_used = 0;
	g_use_rail = 0;

	return 0;
}

/* returns error */
static int rdpup_send(BYTE* data, int len)
{
	int sent;

	LLOGLN(10, ("rdpup_send - sending %d bytes", len));

	if (g_sck_closed)
		return 1;

	while (len > 0)
	{
		sent = g_tcp_send(g_sck, data, len, 0);

		if (sent == -1)
		{
			if (g_tcp_last_error_would_block(g_sck))
			{
				g_sleep(1);
			}
			else
			{
				rdpup_disconnect();
				return 1;
			}
		}
		else if (sent == 0)
		{
			rdpup_disconnect();
			return 1;
		}
		else
		{
			data += sent;
			len -= sent;
		}
	}

	return 0;
}

static int rdpup_send_msg(wStream* s)
{
	int length;
	int status;

	status = 1;

	if (s != 0)
	{
		length = Stream_GetPosition(s);

		if (length > Stream_Capacity(s))
		{
			rdpLog("overrun error len %d count %d\n", length, g_count);
		}

		Stream_SetPosition(s, 0);
		Stream_Write_UINT32(s, length);
		Stream_Write_UINT32(s, g_count);

		status = rdpup_send(Stream_Buffer(s), length);
	}

	if (status != 0)
	{
		rdpLog("error in rdpup_send_msg\n");
	}

	return status;
}

int rdpup_update(XRDP_MSG_COMMON* msg);

/* returns error */
static int rdpup_recv(BYTE* data, int len)
{
	int rcvd;

	if (g_sck_closed)
	{
		return 1;
	}

	while (len > 0)
	{
		rcvd = g_tcp_recv(g_sck, data, len, 0);

		if (rcvd == -1)
		{
			if (g_tcp_last_error_would_block(g_sck))
			{
				g_sleep(1);
			}
			else
			{
				rdpup_disconnect();
				return 1;
			}
		}
		else if (rcvd == 0)
		{
			rdpup_disconnect();
			return 1;
		}
		else
		{
			data += rcvd;
			len -= rcvd;
		}
	}

	return 0;
}

static int rdpup_recv_msg(wStream* s, int* type)
{
	int status = 1;
	XRDP_MSG_COMMON common;

	if (s != 0)
	{
		Stream_EnsureCapacity(s, 10);
		Stream_SetPosition(s, 0);

		status = rdpup_recv(Stream_Pointer(s), 10);

		if (status == 0)
		{
			xrdp_read_common_header(s, &common);

			if (common.length >= 10)
			{
				Stream_EnsureCapacity(s, common.length);
				status = rdpup_recv(Stream_Pointer(s), common.length - 10);
			}
		}
	}

	if (status != 0)
	{
		rdpLog("error in rdpup_recv_msg\n");
	}

	Stream_SetPosition(s, common.length);
	Stream_SealLength(s);
	Stream_SetPosition(s, 10);

	*type = common.type;

	return status;
}

static int process_screen_parameters(int DesktopWidth, int DesktopHeight, int ColorDepth)
{
	Bool ok;
	int mmwidth;
	int mmheight;
	RRScreenSizePtr pSize;

	g_rdpScreen.rdp_width = DesktopWidth;
	g_rdpScreen.rdp_height = DesktopHeight;
	g_rdpScreen.rdp_bpp = ColorDepth;

	if (ColorDepth < 15)
	{
		g_rdpScreen.rdp_Bpp = 1;
		g_rdpScreen.rdp_Bpp_mask = 0xFF;
	}
	else if (ColorDepth == 15)
	{
		g_rdpScreen.rdp_Bpp = 2;
		g_rdpScreen.rdp_Bpp_mask = 0x7FFF;
	}
	else if (ColorDepth == 16)
	{
		g_rdpScreen.rdp_Bpp = 2;
		g_rdpScreen.rdp_Bpp_mask = 0xFFFF;
	}
	else if (ColorDepth > 16)
	{
		g_rdpScreen.rdp_Bpp = 4;
		g_rdpScreen.rdp_Bpp_mask = 0xFFFFFF;
	}

	mmwidth = PixelToMM(DesktopWidth);
	mmheight = PixelToMM(DesktopHeight);

	pSize = RRRegisterSize(g_pScreen, DesktopWidth, DesktopHeight, mmwidth, mmheight);
	RRSetCurrentConfig(g_pScreen, RR_Rotate_0, 0, pSize);

	if ((g_rdpScreen.width != DesktopWidth) || (g_rdpScreen.height != DesktopHeight))
	{
		LLOGLN(0, ("  calling RRScreenSizeSet"));
		ok = RRScreenSizeSet(g_pScreen, DesktopWidth, DesktopHeight, mmwidth, mmheight);
		LLOGLN(0, ("  RRScreenSizeSet ok=[%d]", ok));
	}

	return 0;
}

static int rdpup_process_capabilities_msg(wStream* s)
{
	XRDP_MSG_CAPABILITIES msg;

	xrdp_read_capabilities(s, &msg);

	process_screen_parameters(msg.DesktopWidth, msg.DesktopHeight, msg.ColorDepth);

	return 0;
}

int rdpup_process_refresh_rect_msg(wStream* s, XRDP_MSG_REFRESH_RECT* msg)
{
	int index;

	Stream_Read_UINT16(s, msg->numberOfAreas);

	msg->areasToRefresh = (RECTANGLE_16*) Stream_Pointer(s);

	for (index = 0; index < msg->numberOfAreas; index++)
	{
		Stream_Read_UINT16(s, msg->areasToRefresh[index].left);
		Stream_Read_UINT16(s, msg->areasToRefresh[index].top);
		Stream_Read_UINT16(s, msg->areasToRefresh[index].right);
		Stream_Read_UINT16(s, msg->areasToRefresh[index].bottom);
	}

	return 0;
}

static int rdpup_process_msg(wStream* s, int type)
{
	LLOGLN(10, ("rdpup_process_msg - msg %d", type));

	if (type == XRDP_CLIENT_SYNCHRONIZE_KEYBOARD_EVENT)
	{
		XRDP_MSG_SYNCHRONIZE_KEYBOARD_EVENT msg;

		xrdp_read_synchronize_keyboard_event(s, &msg);
	}
	else if (type == XRDP_CLIENT_SCANCODE_KEYBOARD_EVENT)
	{
		XRDP_MSG_SCANCODE_KEYBOARD_EVENT msg;

		xrdp_read_scancode_keyboard_event(s, &msg);

		KbdAddScancodeEvent(msg.flags, msg.code, msg.keyboardType);
	}
	else if (type == XRDP_CLIENT_VIRTUAL_KEYBOARD_EVENT)
	{
		XRDP_MSG_VIRTUAL_KEYBOARD_EVENT msg;

		xrdp_read_virtual_keyboard_event(s, &msg);

		KbdAddVirtualKeyCodeEvent(msg.flags, msg.code);
	}
	else if (type == XRDP_CLIENT_UNICODE_KEYBOARD_EVENT)
	{
		XRDP_MSG_UNICODE_KEYBOARD_EVENT msg;

		xrdp_read_unicode_keyboard_event(s, &msg);

		KbdAddUnicodeEvent(msg.flags, msg.code);
	}
	else if (type == XRDP_CLIENT_MOUSE_EVENT)
	{
		XRDP_MSG_MOUSE_EVENT msg;

		xrdp_read_mouse_event(s, &msg);

		if (msg.x > g_rdpScreen.width - 2)
			msg.x = g_rdpScreen.width - 2;

		if (msg.y > g_rdpScreen.height - 2)
			msg.y = g_rdpScreen.height - 2;

		if (msg.flags & PTR_FLAGS_MOVE)
		{
			PtrAddEvent(g_button_mask, msg.x, msg.y);
		}

		if (msg.flags & PTR_FLAGS_WHEEL)
		{
			if (msg.flags & PTR_FLAGS_WHEEL_NEGATIVE)
			{
				g_button_mask = g_button_mask | 16;
				PtrAddEvent(g_button_mask, msg.x, msg.y);

				g_button_mask = g_button_mask & (~16);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
			else
			{
				g_button_mask = g_button_mask | 8;
				PtrAddEvent(g_button_mask, msg.x, msg.y);

				g_button_mask = g_button_mask & (~8);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
		}
		else if (msg.flags & PTR_FLAGS_BUTTON1)
		{
			if (msg.flags & PTR_FLAGS_DOWN)
			{
				g_button_mask = g_button_mask | 1;
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
			else
			{
				g_button_mask = g_button_mask & (~1);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
		}
		else if (msg.flags & PTR_FLAGS_BUTTON2)
		{
			if (msg.flags & PTR_FLAGS_DOWN)
			{
				g_button_mask = g_button_mask | 4;
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
			else
			{
				g_button_mask = g_button_mask & (~4);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
		}
		else if (msg.flags & PTR_FLAGS_BUTTON3)
		{
			if (msg.flags & PTR_FLAGS_DOWN)
			{
				g_button_mask = g_button_mask | 2;
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
			else
			{
				g_button_mask = g_button_mask & (~2);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
		}
	}
	else if (type == XRDP_CLIENT_EXTENDED_MOUSE_EVENT)
	{
		XRDP_MSG_EXTENDED_MOUSE_EVENT msg;

		xrdp_read_extended_mouse_event(s, &msg);

		if (msg.x > g_rdpScreen.width - 2)
			msg.x = g_rdpScreen.width - 2;

		if (msg.y > g_rdpScreen.height - 2)
			msg.y = g_rdpScreen.height - 2;

		if (msg.flags & PTR_FLAGS_MOVE)
		{
			PtrAddEvent(g_button_mask, msg.x, msg.y);
		}

		if (msg.flags & PTR_XFLAGS_BUTTON1)
		{
			if (msg.flags & PTR_XFLAGS_DOWN)
			{
				g_button_mask = g_button_mask | 8;
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
			else
			{
				g_button_mask = g_button_mask & (~8);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
		}
		else if (msg.flags & PTR_XFLAGS_BUTTON2)
		{
			if (msg.flags & PTR_XFLAGS_DOWN)
			{
				g_button_mask = g_button_mask | 16;
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
			else
			{
				g_button_mask = g_button_mask & (~16);
				PtrAddEvent(g_button_mask, msg.x, msg.y);
			}
		}
	}
	else if (type == XRDP_CLIENT_CAPABILITIES)
	{
		rdpup_process_capabilities_msg(s);
	}
	else if (type == XRDP_CLIENT_REFRESH_RECT)
	{
		int index;
		XRDP_MSG_REFRESH_RECT msg;

		xrdp_read_refresh_rect(s, &msg);

		rdpup_begin_update();

		for (index = 0; index < msg.numberOfAreas; index++)
		{
			rdpup_send_area(msg.areasToRefresh[index].left, msg.areasToRefresh[index].top,
					msg.areasToRefresh[index].right - msg.areasToRefresh[index].left + 1,
					msg.areasToRefresh[index].bottom - msg.areasToRefresh[index].top + 1);
		}

		rdpup_end_update();
	}
	else
	{
		rdpLog("unknown message type in rdpup_process_msg %d\n", type);
	}

	return 0;
}

void rdpup_get_screen_image_rect(struct image_data *id)
{
	id->width = g_rdpScreen.width;
	id->height = g_rdpScreen.height;
	id->bpp = g_rdpScreen.rdp_bpp;
	id->Bpp = g_rdpScreen.rdp_Bpp;
	id->lineBytes = g_rdpScreen.paddedWidthInBytes;
	id->pixels = g_rdpScreen.pfbMemory;
}

UINT32 rdpup_convert_color(UINT32 color)
{
	color = color & g_Bpp_mask;
	color = convert_pixel(color) & g_rdpScreen.rdp_Bpp_mask;
	return color;
}

UINT32 rdpup_convert_opcode(int opcode)
{
	return g_rdp_opcodes[opcode & 0xF];
}

UINT32 rdp_dstblt_rop(int opcode)
{
	UINT32 rop = 0;

	switch (opcode)
	{
		case GXclear:
			rop = GDI_BLACKNESS;
			break;

		case GXset:
			rop = GDI_WHITENESS;
			break;

		case GXnoop:
			rop = GDI_D;
			break;

		case GXinvert:
			rop = GDI_DSTINVERT;
			break;

		default:
			rop = 0;
			break;
	}

	return rop;
}

int rdpup_begin_update(void)
{
	XRDP_MSG_BEGIN_UPDATE msg;

	if (g_connected)
	{
		if (g_begin)
		{
			return 0;
		}

		Stream_SetPosition(g_out_s, 0);
		Stream_Seek(g_out_s, 8);

		LLOGLN(10, ("begin %d", g_count));

		msg.type = XRDP_SERVER_BEGIN_UPDATE;
		rdpup_update((XRDP_MSG_COMMON*) &msg);
	}

	return 0;
}

int rdpup_end_update(void)
{
	XRDP_MSG_END_UPDATE msg;

	LLOGLN(10, ("rdpup_end_update"));

	if (g_connected && g_begin)
	{
		msg.type = XRDP_SERVER_END_UPDATE;
		rdpup_update((XRDP_MSG_COMMON*) &msg);
	}

	return 0;
}

int rdpup_update(XRDP_MSG_COMMON* msg)
{
	return 0;
}

int rdpup_check_attach_framebuffer()
{
	if (g_rdpScreen.sharedMemory && !g_rdpScreen.fbAttached)
	{
		XRDP_MSG_SHARED_FRAMEBUFFER msg;

		msg.attach = 1;
		msg.width = g_rdpScreen.width;
		msg.height = g_rdpScreen.height;
		msg.scanline = g_rdpScreen.paddedWidthInBytes;
		msg.segmentId = g_rdpScreen.segmentId;
		msg.bitsPerPixel = g_rdpScreen.depth;
		msg.bytesPerPixel = g_Bpp;

		msg.type = XRDP_SERVER_SHARED_FRAMEBUFFER;
		service->server->SharedFramebuffer(service->server, &msg);

		g_rdpScreen.fbAttached = 1;
	}

	return 0;
}

int rdpup_opaque_rect(XRDP_MSG_OPAQUE_RECT* msg)
{
	rdpup_check_attach_framebuffer();

	msg->type = XRDP_SERVER_OPAQUE_RECT;
	service->server->OpaqueRect(service->server, &msg);

	return 0;
}

int rdpup_screen_blt(short x, short y, int cx, int cy, short srcx, short srcy)
{
	XRDP_MSG_SCREEN_BLT msg;

	rdpup_check_attach_framebuffer();

	msg.nLeftRect = x;
	msg.nTopRect = y;
	msg.nWidth = cx;
	msg.nHeight = cy;
	msg.nXSrc = srcx;
	msg.nYSrc = srcy;

	msg.type = XRDP_SERVER_SCREEN_BLT;
	service->server->ScreenBlt(service->server, &msg);

	return 0;
}

int rdpup_patblt(XRDP_MSG_PATBLT* msg)
{
	rdpup_check_attach_framebuffer();

	msg->type = XRDP_SERVER_PATBLT;
	service->server->PatBlt(service->server, &msg);

	return 0;
}

int rdpup_dstblt(XRDP_MSG_DSTBLT* msg)
{
	rdpup_check_attach_framebuffer();

	msg->type = XRDP_SERVER_DSTBLT;
	service->server->DstBlt(service->server, &msg);

	return 0;
}

int rdpup_set_clipping_region(XRDP_MSG_SET_CLIPPING_REGION* msg)
{
	msg->type = XRDP_SERVER_SET_CLIPPING_REGION;
	service->server->SetClippingRegion(service->server, &msg);
	return 0;
}

int rdpup_set_clip(short x, short y, int cx, int cy)
{
	XRDP_MSG_SET_CLIPPING_REGION msg;

	msg.bNullRegion = 0;
	msg.nLeftRect = x;
	msg.nTopRect = y;
	msg.nWidth = cx;
	msg.nHeight = cy;

	service->server->SetClippingRegion(service->server, &msg);

	return 0;
}

int rdpup_reset_clip(void)
{
	XRDP_MSG_SET_CLIPPING_REGION msg;

	msg.bNullRegion = 1;
	msg.nLeftRect = 0;
	msg.nTopRect = 0;
	msg.nWidth = 0;
	msg.nHeight = 0;

	service->server->SetClippingRegion(service->server, &msg);

	return 0;
}

int rdpup_draw_line(XRDP_MSG_LINE_TO* msg)
{
	msg->type = XRDP_SERVER_LINE_TO;
	service->server->LineTo(service->server, &msg);
	return 0;
}

int rdpup_set_pointer(XRDP_MSG_SET_POINTER* msg)
{
	msg->type = XRDP_SERVER_SET_POINTER;
	service->server->SetPointer(service->server, &msg);
	return 0;
}

void rdpup_send_area(int x, int y, int w, int h)
{
	int bitmapLength;
	XRDP_MSG_PAINT_RECT msg;

	if (x < 0)
		x = 0;

	if (x > g_rdpScreen.width - 1)
		x = g_rdpScreen.width - 1;

	w += x % 16;
	x -= x % 16;

	w += w % 16;

	if (x + w > g_rdpScreen.width)
		w = g_rdpScreen.width - x;

	if (y < 0)
		y = 0;

	if (y > g_rdpScreen.height - 1)
		y = g_rdpScreen.height - 1;

	h += y % 16;
	y -= y % 16;

	h += h % 16;

	if (h > g_rdpScreen.height)
		h = g_rdpScreen.height;

	if (y + h > g_rdpScreen.height)
		h = g_rdpScreen.height - y;

	if (w * h < 1)
		return;

	bitmapLength = w * h * g_Bpp;

	if (g_rdpScreen.sharedMemory && !g_rdpScreen.fbAttached)
	{
		XRDP_MSG_SHARED_FRAMEBUFFER msg;

		msg.attach = 1;
		msg.width = g_rdpScreen.width;
		msg.height = g_rdpScreen.height;
		msg.scanline = g_rdpScreen.paddedWidthInBytes;
		msg.segmentId = g_rdpScreen.segmentId;
		msg.bitsPerPixel = g_rdpScreen.depth;
		msg.bytesPerPixel = g_Bpp;

		msg.type = XRDP_SERVER_SHARED_FRAMEBUFFER;
		rdpup_update((XRDP_MSG_COMMON*) &msg);

		g_rdpScreen.fbAttached = 1;
	}

	msg.nLeftRect = x;
	msg.nTopRect = y;
	msg.nWidth = w;
	msg.nHeight = h;
	msg.nXSrc = 0;
	msg.nYSrc = 0;

	msg.fbSegmentId = g_rdpScreen.segmentId;
	msg.bitmapData = NULL;
	msg.bitmapDataLength = 0;

	msg.type = XRDP_SERVER_PAINT_RECT;
	service->server->PaintRect(service->server, &msg);
}

void rdpup_shared_framebuffer(XRDP_MSG_SHARED_FRAMEBUFFER* msg)
{
	msg->type = XRDP_SERVER_SHARED_FRAMEBUFFER;
	service->server->SharedFramebuffer(service->server, msg);
}

void rdpup_create_window(WindowPtr pWindow, rdpWindowRec *priv)
{
	RECTANGLE_16 windowRects;
	RECTANGLE_16 visibilityRects;
	XRDP_MSG_WINDOW_NEW_UPDATE msg;

	msg.rootParentHandle = (UINT32) pWindow->drawable.pScreen->root->drawable.id;

	if (pWindow->overrideRedirect)
	{
		msg.style = XR_STYLE_TOOLTIP;
		msg.extendedStyle = XR_EXT_STYLE_TOOLTIP;
	}
	else
	{
		msg.style = XR_STYLE_NORMAL;
		msg.extendedStyle = XR_EXT_STYLE_NORMAL;
	}

	msg.titleInfo.string = (BYTE*) _strdup("title");
	msg.titleInfo.length = strlen((char*) msg.titleInfo.string);

	msg.windowId = (UINT32) pWindow->drawable.id;
	msg.ownerWindowId = (UINT32) pWindow->parent->drawable.id;

	msg.showState = 0;

	msg.clientOffsetX = 0;
	msg.clientOffsetY = 0;

	msg.clientAreaWidth = pWindow->drawable.width;
	msg.clientAreaHeight = pWindow->drawable.height;

	msg.RPContent = 0;

	msg.windowOffsetX = pWindow->drawable.x;
	msg.windowOffsetY = pWindow->drawable.y;

	msg.windowClientDeltaX = 0;
	msg.windowClientDeltaY = 0;

	msg.windowWidth = pWindow->drawable.width;
	msg.windowHeight = pWindow->drawable.height;

	msg.numWindowRects = 1;
	msg.windowRects = (RECTANGLE_16*) &windowRects;
	msg.windowRects[0].left = 0;
	msg.windowRects[0].top = 0;
	msg.windowRects[0].right = pWindow->drawable.width;
	msg.windowRects[0].bottom = pWindow->drawable.height;

	msg.numVisibilityRects = 1;
	msg.visibilityRects = (RECTANGLE_16*) &visibilityRects;
	msg.visibilityRects[0].left = 0;
	msg.visibilityRects[0].top = 0;
	msg.visibilityRects[0].right = pWindow->drawable.width;
	msg.visibilityRects[0].bottom = pWindow->drawable.height;

	msg.visibleOffsetX = pWindow->drawable.x;
	msg.visibleOffsetY = pWindow->drawable.y;

	msg.type = XRDP_SERVER_WINDOW_NEW_UPDATE;
	service->server->WindowNewUpdate(service->server, &msg);

	free(msg.titleInfo.string);
}

void rdpup_delete_window(WindowPtr pWindow, rdpWindowRec *priv)
{
	XRDP_MSG_WINDOW_DELETE msg;

	msg.windowId = (UINT32) pWindow->drawable.id;

	msg.type = XRDP_SERVER_WINDOW_DELETE;
	service->server->WindowDelete(service->server, &msg);
}

int rds_client_synchronize_keyboard_event(rdsService* service, DWORD flags)
{
	return 0;
}

int rds_client_scancode_keyboard_event(rdsService* service, DWORD flags, DWORD code, DWORD keyboardType)
{
	KbdAddScancodeEvent(flags, code, keyboardType);
	return 0;
}

int rds_client_virtual_keyboard_event(rdsService* service, DWORD flags, DWORD code)
{
	KbdAddVirtualKeyCodeEvent(flags, code);
	return 0;
}

int rds_client_unicode_keyboard_event(rdsService* service, DWORD flags, DWORD code)
{
	KbdAddUnicodeEvent(flags, code);
	return 0;
}

int rds_client_mouse_event(rdsService* service, DWORD flags, DWORD x, DWORD y)
{
	if (x > g_rdpScreen.width - 2)
		x = g_rdpScreen.width - 2;

	if (y > g_rdpScreen.height - 2)
		y = g_rdpScreen.height - 2;

	if (flags & PTR_FLAGS_MOVE)
	{
		PtrAddEvent(g_button_mask, x, y);
	}

	if (flags & PTR_FLAGS_WHEEL)
	{
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
		{
			g_button_mask = g_button_mask | 16;
			PtrAddEvent(g_button_mask, x, y);

			g_button_mask = g_button_mask & (~16);
			PtrAddEvent(g_button_mask, x, y);
		}
		else
		{
			g_button_mask = g_button_mask | 8;
			PtrAddEvent(g_button_mask, x, y);

			g_button_mask = g_button_mask & (~8);
			PtrAddEvent(g_button_mask, x, y);
		}
	}
	else if (flags & PTR_FLAGS_BUTTON1)
	{
		if (flags & PTR_FLAGS_DOWN)
		{
			g_button_mask = g_button_mask | 1;
			PtrAddEvent(g_button_mask, x, y);
		}
		else
		{
			g_button_mask = g_button_mask & (~1);
			PtrAddEvent(g_button_mask, x, y);
		}
	}
	else if (flags & PTR_FLAGS_BUTTON2)
	{
		if (flags & PTR_FLAGS_DOWN)
		{
			g_button_mask = g_button_mask | 4;
			PtrAddEvent(g_button_mask, x, y);
		}
		else
		{
			g_button_mask = g_button_mask & (~4);
			PtrAddEvent(g_button_mask, x, y);
		}
	}
	else if (flags & PTR_FLAGS_BUTTON3)
	{
		if (flags & PTR_FLAGS_DOWN)
		{
			g_button_mask = g_button_mask | 2;
			PtrAddEvent(g_button_mask, x, y);
		}
		else
		{
			g_button_mask = g_button_mask & (~2);
			PtrAddEvent(g_button_mask, x, y);
		}
	}

	return 0;
}

int rds_client_extended_mouse_event(rdsService* service, DWORD flags, DWORD x, DWORD y)
{
	if (x > g_rdpScreen.width - 2)
		x = g_rdpScreen.width - 2;

	if (y > g_rdpScreen.height - 2)
		y = g_rdpScreen.height - 2;

	if (flags & PTR_FLAGS_MOVE)
	{
		PtrAddEvent(g_button_mask, x, y);
	}

	if (flags & PTR_XFLAGS_BUTTON1)
	{
		if (flags & PTR_XFLAGS_DOWN)
		{
			g_button_mask = g_button_mask | 8;
			PtrAddEvent(g_button_mask, x, y);
		}
		else
		{
			g_button_mask = g_button_mask & (~8);
			PtrAddEvent(g_button_mask, x, y);
		}
	}
	else if (flags & PTR_XFLAGS_BUTTON2)
	{
		if (flags & PTR_XFLAGS_DOWN)
		{
			g_button_mask = g_button_mask | 16;
			PtrAddEvent(g_button_mask, x, y);
		}
		else
		{
			g_button_mask = g_button_mask & (~16);
			PtrAddEvent(g_button_mask, x, y);
		}
	}

	return 0;
}

int rds_service_accept(rdsService* service)
{
	g_clientfd = GetNamePipeFileDescriptor(service->hClientPipe);

	g_begin = 0;
	g_con_number++;
	g_connected = 1;
	g_rdpScreen.fbAttached = 0;
	AddEnabledDevice(g_clientfd);

	fprintf(stderr, "RdsServiceAccept\n");

	return 0;
}

int rdpup_init(void)
{
	int DisplayId;

	DisplayId = atoi(display);

	LLOGLN(0, ("rdpup_init: display: %d", DisplayId));

	if (DisplayId < 1)
	{
		return 0;
	}

	if (g_in_s == 0)
	{
		g_in_s = Stream_New(NULL, 8192);
	}

	if (g_out_s == 0)
	{
		g_out_s = Stream_New(NULL, 1920 * 1088 * g_Bpp + 100);
	}

	pfbBackBufferMemory = (BYTE*) malloc(g_rdpScreen.sizeInBytes);

	if (!service)
	{
		service = freerds_service_new(DisplayId, "X11rdp");

		service->Accept = (pRdsServiceAccept) rds_service_accept;

		service->client->SynchronizeKeyboardEvent = rds_client_synchronize_keyboard_event;
		service->client->ScancodeKeyboardEvent = rds_client_scancode_keyboard_event;
		service->client->VirtualKeyboardEvent = rds_client_virtual_keyboard_event;
		service->client->UnicodeKeyboardEvent = rds_client_unicode_keyboard_event;
		service->client->MouseEvent = rds_client_mouse_event;
		service->client->ExtendedMouseEvent = rds_client_extended_mouse_event;

		freerds_service_start(service);
	}

	return 1;
}

int rdpup_check(void)
{
	if (service->hClientPipe)
	{
		if (WaitForSingleObject(service->hClientPipe, 0) == WAIT_OBJECT_0)
		{
			freerds_transport_receive((rdsModule*) service);
		}
	}

	return 0;
}
