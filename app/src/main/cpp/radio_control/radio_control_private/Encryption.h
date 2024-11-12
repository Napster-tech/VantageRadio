#pragma once

#include "ByteArray.h"

enum { BlowfishBlockSize = 8 };

bool blowfishEncrypt(const ByteArray& key, ByteArray* data);
bool blowfishDecrypt(const ByteArray& key, ByteArray* data, size_t unencryptedSize);
