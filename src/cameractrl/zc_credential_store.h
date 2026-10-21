/*
obs-zcamera
 Copyright (C) 2019-2020 Yibai Zhang
 Copyright (C) 2026 ZCamera OBS plugin contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include "zc_http_transport.h"

namespace zc {

/* Per-host camera logins.

   Two layers:

   * the session layer — what this run has used so far. The dock fills it in
     when the operator logs a camera in; the source's Properties bitrate push
     reads it, because a source has no login UI of its own.

   * the persisted layer — a camera login that the camera itself accepted is
     written to `<OBS plugin config>/obs-zcamera/credentials.json`, so the
     operator types it once per camera instead of once per OBS start.

   The password is never stored as plain text: on Windows it is sealed with
   DPAPI (CryptProtectData, current-user scope), so the blob only opens for the
   same Windows account on the same machine. Builds without a platform keystore
   (the macOS one) persist the user name only and keep the password in memory
   for the session, rather than writing a secret to a file.

   Everything here is called from the OBS UI thread (the dock and the source
   updates both run there), so a plain function-local static is enough. */

/* Remember credentials for this session only. */
void rememberCredentials(const QString &host, const ZcCredentials &creds);

/* Remember them for this session and on disk, after the camera accepted them. */
void saveCredentials(const QString &host, const ZcCredentials &creds);

/* Drop a host's login from memory and from disk — used when the camera
   rejected a stored login, so the next start prompts instead of retrying a
   password the camera has already refused. */
void forgetCredentials(const QString &host);

/* Session credentials first, then whatever a previous run saved. */
ZcCredentials lookupCredentials(const QString &host);

} // namespace zc
