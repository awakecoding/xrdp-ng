/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * xrdp-ng interprocess communication protocol
 *
 * Copyright 2013 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xrdp-ng/xrdp.h>

#include <winpr/crt.h>
#include <winpr/file.h>
#include <winpr/pipe.h>
#include <winpr/path.h>

#include "transport.h"

int freerds_named_pipe_read(HANDLE hNamedPipe, BYTE* data, DWORD length)
{
	BOOL fSuccess = FALSE;
	DWORD NumberOfBytesRead;
	DWORD TotalNumberOfBytesRead = 0;

	if (!hNamedPipe)
		return -1;

	NumberOfBytesRead = 0;

	fSuccess = ReadFile(hNamedPipe, data, length, &NumberOfBytesRead, NULL);

	if (!fSuccess || (NumberOfBytesRead == 0))
	{
		return -1;
	}

	TotalNumberOfBytesRead += NumberOfBytesRead;
	length -= NumberOfBytesRead;
	data += NumberOfBytesRead;

	return TotalNumberOfBytesRead;
}

int freerds_named_pipe_write(HANDLE hNamedPipe, BYTE* data, DWORD length)
{
	BOOL fSuccess = FALSE;
	DWORD NumberOfBytesWritten;
	DWORD TotalNumberOfBytesWritten = 0;

	if (!hNamedPipe)
		return -1;

	while (length > 0)
	{
		NumberOfBytesWritten = 0;

		fSuccess = WriteFile(hNamedPipe, data, length, &NumberOfBytesWritten, NULL);

		if (!fSuccess || (NumberOfBytesWritten == 0))
		{
			return -1;
		}

		TotalNumberOfBytesWritten += NumberOfBytesWritten;
		length -= NumberOfBytesWritten;
		data += NumberOfBytesWritten;
	}

	return NumberOfBytesWritten;
}

int freerds_named_pipe_clean(DWORD SessionId, const char* endpoint)
{
	int status = 0;
	char* filename;
	char pipeName[256];

	sprintf_s(pipeName, sizeof(pipeName), "\\\\.\\pipe\\FreeRDS_%d_%s", (int) SessionId, endpoint);

	filename = GetNamedPipeUnixDomainSocketFilePathA(pipeName);

	if (PathFileExistsA(filename))
	{
		DeleteFileA(filename);
		status = 1;
	}

	free(filename);

	return status;
}

HANDLE freerds_named_pipe_connect(DWORD SessionId, const char* endpoint, DWORD nTimeOut)
{
	HANDLE hNamedPipe;
	char pipeName[256];

	sprintf_s(pipeName, sizeof(pipeName), "\\\\.\\pipe\\FreeRDS_%d_%s", (int) SessionId, endpoint);

	if (!WaitNamedPipeA(pipeName, nTimeOut))
	{
		fprintf(stderr, "WaitNamedPipe failure: %s\n", pipeName);
		return NULL;
	}

	hNamedPipe = CreateFileA(pipeName,
			GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

	if ((!hNamedPipe) || (hNamedPipe == INVALID_HANDLE_VALUE))
	{
		fprintf(stderr, "Failed to create named pipe %s\n", pipeName);
		return NULL;
	}

	return hNamedPipe;
}

HANDLE freerds_named_pipe_create(DWORD SessionId, const char* endpoint)
{
	HANDLE hNamedPipe;
	char pipeName[256];

	sprintf_s(pipeName, sizeof(pipeName), "\\\\.\\pipe\\FreeRDS_%d_%s", (int) SessionId, endpoint);

	hNamedPipe = CreateNamedPipe(pipeName, PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0, NULL);

	fprintf(stderr, "Creating Named Pipe: %s\n", pipeName);

	if ((!hNamedPipe) || (hNamedPipe == INVALID_HANDLE_VALUE))
	{
		fprintf(stderr, "CreateNamedPipe failure\n");
		return NULL;
	}

	return hNamedPipe;
}

HANDLE freerds_named_pipe_accept(HANDLE hServerPipe)
{
	BOOL fConnected;
	DWORD dwPipeMode;
	HANDLE hClientPipe;

	fConnected = ConnectNamedPipe(hServerPipe, NULL);

	if (!fConnected)
		fConnected = (GetLastError() == ERROR_PIPE_CONNECTED);

	if (!fConnected)
	{
		return NULL;
	}

	hClientPipe = hServerPipe;

	dwPipeMode = PIPE_WAIT;
	SetNamedPipeHandleState(hClientPipe, &dwPipeMode, NULL, NULL);

	return hClientPipe;
}

int freerds_receive_server_message(rdsModule* mod, wStream* s, XRDP_MSG_COMMON* common)
{
	int status = 0;
	rdsServerInterface* server;

	server = mod->server;

	switch (common->type)
	{
		case XRDP_SERVER_BEGIN_UPDATE:
			{
				XRDP_MSG_BEGIN_UPDATE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->BeginUpdate(mod, &msg);
			}
			break;

		case XRDP_SERVER_END_UPDATE:
			{
				XRDP_MSG_END_UPDATE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->EndUpdate(mod, &msg);
			}
			break;

		case XRDP_SERVER_OPAQUE_RECT:
			{
				XRDP_MSG_OPAQUE_RECT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->OpaqueRect(mod, &msg);
			}
			break;

		case XRDP_SERVER_SCREEN_BLT:
			{
				XRDP_MSG_SCREEN_BLT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->ScreenBlt(mod, &msg);
			}
			break;

		case XRDP_SERVER_PATBLT:
			{
				XRDP_MSG_PATBLT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->PatBlt(mod, &msg);
			}
			break;

		case XRDP_SERVER_DSTBLT:
			{
				XRDP_MSG_DSTBLT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->DstBlt(mod, &msg);
			}
			break;

		case XRDP_SERVER_PAINT_RECT:
			{
				int status;
				XRDP_MSG_PAINT_RECT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));

				msg.fbSegmentId = 0;
				msg.framebuffer = NULL;

				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);

				if (msg.fbSegmentId)
					msg.framebuffer = &(mod->framebuffer);

				status = server->PaintRect(mod, &msg);
			}
			break;

		case XRDP_SERVER_SET_CLIPPING_REGION:
			{
				XRDP_MSG_SET_CLIPPING_REGION msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->SetClippingRegion(mod, &msg);
			}
			break;

		case XRDP_SERVER_LINE_TO:
			{
				XRDP_MSG_LINE_TO msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->LineTo(mod, &msg);
			}
			break;

		case XRDP_SERVER_SET_POINTER:
			{
				XRDP_MSG_SET_POINTER msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->SetPointer(mod, &msg);
			}
			break;

		case XRDP_SERVER_CREATE_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_CREATE_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->CreateOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_SWITCH_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_SWITCH_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->SwitchOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_DELETE_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_DELETE_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->DeleteOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_PAINT_OFFSCREEN_SURFACE:
			{
				XRDP_MSG_PAINT_OFFSCREEN_SURFACE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->PaintOffscreenSurface(mod, &msg);
			}
			break;

		case XRDP_SERVER_WINDOW_NEW_UPDATE:
			{
				XRDP_MSG_WINDOW_NEW_UPDATE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->WindowNewUpdate(mod, &msg);
			}
			break;

		case XRDP_SERVER_WINDOW_DELETE:
			{
				XRDP_MSG_WINDOW_DELETE msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->WindowDelete(mod, &msg);
			}
			break;

		case XRDP_SERVER_SHARED_FRAMEBUFFER:
			{
				XRDP_MSG_SHARED_FRAMEBUFFER msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_server_message_read(s, (XRDP_MSG_COMMON*) &msg);
				status = server->SharedFramebuffer(mod, &msg);
			}
			break;

		default:
			status = 0;
			break;
	}

	return status;
}

int freerds_receive_client_message(rdsModule* mod, wStream* s, XRDP_MSG_COMMON* common)
{
	int status = 0;
	rdsClientInterface* client;

	client = mod->client;

	switch (common->type)
	{
		case XRDP_CLIENT_SYNCHRONIZE_KEYBOARD_EVENT:
			{
				XRDP_MSG_SYNCHRONIZE_KEYBOARD_EVENT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_read_synchronize_keyboard_event(s, &msg);
				status = client->SynchronizeKeyboardEvent(mod, msg.flags);
			}
			break;

		case XRDP_CLIENT_SCANCODE_KEYBOARD_EVENT:
			{
				XRDP_MSG_SCANCODE_KEYBOARD_EVENT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_read_scancode_keyboard_event(s, &msg);
				status = client->ScancodeKeyboardEvent(mod, msg.flags, msg.code, msg.keyboardType);
			}
			break;

		case XRDP_CLIENT_VIRTUAL_KEYBOARD_EVENT:
			{
				XRDP_MSG_VIRTUAL_KEYBOARD_EVENT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_read_virtual_keyboard_event(s, &msg);
				status = client->VirtualKeyboardEvent(mod, msg.flags, msg.code);
			}
			break;

		case XRDP_CLIENT_UNICODE_KEYBOARD_EVENT:
			{
				XRDP_MSG_UNICODE_KEYBOARD_EVENT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_read_unicode_keyboard_event(s, &msg);
				status = client->UnicodeKeyboardEvent(mod, msg.flags, msg.code);
			}
			break;

		case XRDP_CLIENT_MOUSE_EVENT:
			{
				XRDP_MSG_MOUSE_EVENT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_read_mouse_event(s, &msg);
				status = client->MouseEvent(mod, msg.flags, msg.x, msg.y);
			}
			break;

		case XRDP_CLIENT_EXTENDED_MOUSE_EVENT:
			{
				XRDP_MSG_EXTENDED_MOUSE_EVENT msg;
				CopyMemory(&msg, common, sizeof(XRDP_MSG_COMMON));
				xrdp_read_extended_mouse_event(s, &msg);
				status = client->ExtendedMouseEvent(mod, msg.flags, msg.x, msg.y);
			}
			break;

		default:
			status = 0;
			break;
	}

	return status;
}

int freerds_receive_message(rdsModule* module, wStream* s, XRDP_MSG_COMMON* common)
{
	if (module->ServerMode)
		return freerds_receive_client_message(module, s, common);
	else
		return freerds_receive_server_message(module, s, common);
}

int freerds_transport_receive(rdsModule* module)
{
	wStream* s;
	int index;
	int status;
	int position;

	s = module->InboundStream;

	if (Stream_GetPosition(s) < 8)
	{
		status = freerds_named_pipe_read(module->hClientPipe, Stream_Pointer(s), 8 - Stream_GetPosition(s));

		if (status > 0)
			Stream_Seek(s, status);

		if (Stream_GetPosition(s) >= 8)
		{
			position = Stream_GetPosition(s);
			Stream_SetPosition(s, 0);

			Stream_Read_UINT32(s, module->InboundTotalLength);
			Stream_Read_UINT32(s, module->InboundTotalCount);

			Stream_SetPosition(s, position);

			Stream_EnsureCapacity(s, module->InboundTotalLength);
		}
	}

	if (Stream_GetPosition(s) >= 8)
	{
		status = freerds_named_pipe_read(module->hClientPipe, Stream_Pointer(s),
				module->InboundTotalLength - Stream_GetPosition(s));

		if (status > 0)
			Stream_Seek(s, status);
	}

	if (Stream_GetPosition(s) >= module->InboundTotalLength)
	{
		Stream_SetPosition(s, 8);

		for (index = 0; index < module->InboundTotalCount; index++)
		{
			XRDP_MSG_COMMON common;

			position = Stream_GetPosition(s);

			xrdp_read_common_header(s, &common);

			status = freerds_receive_message(module, s, &common);

			if (status != 0)
			{
				break;
			}

			Stream_SetPosition(s, position + common.length);
		}

		Stream_SetPosition(s, 0);
		module->InboundTotalLength = 0;
		module->InboundTotalCount = 0;
	}

	return 0;
}
