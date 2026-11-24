/*
 * trimcheck.c
 *
 * Порт trimcheck.d на C.
 *
 * Лицензия: Mozilla Public License 2.0 (как у оригинала).
 * Оригинал: Vladimir Panteleev <vladimir@thecybershadow.net>
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winioctl.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <shlobj.h>
#include <propsys.h>

#define DATAFILENAME   L"trimcheck.bin"
#define SAVEFILENAME   L"trimcheck-cont.json"
#define MB            (1024 * 1024)
#define PADDINGSIZE_MB 32
#define WENFORCE(expr, msg)  do { if (!(expr)) dieLastError(msg); } while (0)
#define ENFORCE(expr, msg)   do { if (!(expr)) die(msg); } while (0)
#define COLOR_RED    (FOREGROUND_RED   | FOREGROUND_INTENSITY)
#define COLOR_GREEN  (FOREGROUND_GREEN | FOREGROUND_INTENSITY)

typedef enum {
  TRB_UNKNOWN = 0,  /* could not determine */
  TRB_NONDET  = 1,  /* non-determinate read after TRIM */
  TRB_DRAT    = 2,  /* deterministic after TRIM, but not zeros */
  TRB_RZAT    = 3,  /* reads after TRIM return zeros */
  TRB_FFAT    = 4   /* reads after TRIM return 0xFF (NVMe) */
} TrimReadBehavior;

typedef struct SaveData {
  wchar_t *ntDrivePath;
  ULONGLONG        offset;
  BYTE *rndBuffer;
  size_t           rndSize;
  TrimReadBehavior behavior;
} SaveData;

static jmp_buf g_exitJmp;
static HANDLE  g_hTestFile = NULL;
static int     g_hTestFileOpen = 0;
static wchar_t g_failureHint[512] = L"";

/* ---------- ошибки ---------- */

static void die(const wchar_t *msg) {
  if (g_failureHint[0]) {
    wprintf(L"%s", g_failureHint);
    g_failureHint[0] = 0;
  }
  /* Аналог D-шного scope(exit): закрыть и удалить тестовый файл при аварии */
  if (g_hTestFileOpen) {
    g_hTestFileOpen = 0;
    CloseHandle(g_hTestFile);
    DeleteFileW(DATAFILENAME);
  }
  wprintf(L"Error: %s\n", msg);
  longjmp(g_exitJmp, 1);
}

static void dieLastError(const wchar_t *msg) {
  DWORD e = GetLastError();
  wchar_t buf[1024];
  _snwprintf(buf, 1023, L"%s (GetLastError = %lu)", msg, e);
  buf[1023] = 0;
  die(buf);
}

static void diefmt(const wchar_t *fmt, ...) {
  wchar_t buf[1024];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf(buf, 1023, fmt, args);
  buf[1023] = 0;
  va_end(args);
  die(buf);
}

/* ---------- вспомогательные функции ---------- */

static void wprintColor(WORD attr, const wchar_t *s) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info;
  WORD old = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  if (GetConsoleScreenBufferInfo(h, &info))
    old = info.wAttributes;
  SetConsoleTextAttribute(h, attr);
  wprintf(L"%s", s);
  SetConsoleTextAttribute(h, old);
}

static int getBitLockerStatus(wchar_t driveLetter, DWORD *outStatus) {
  static PROPERTYKEY fallbackKey = { {0x2D15A9A1, 0xA556, 0x4189,{0x91, 0xAD, 0x02, 0x74, 0x58, 0xF1, 0x1A, 0x07}}, 1717 };
  wchar_t parsingName[3];
  IShellItem2 *pDrive = NULL;
  PROPERTYKEY pKey;
  PROPVARIANT prop;
  HRESULT hr;

  parsingName[0] = driveLetter;
  parsingName[1] = L':';
  parsingName[2] = L'\0';

  hr = SHCreateItemFromParsingName(parsingName, NULL, &IID_IShellItem2, (void **)&pDrive);
  if (FAILED(hr)) return -1;

  hr = PSGetPropertyKeyFromName(L"System.Volume.BitLockerProtection", &pKey);
  if (FAILED(hr)) pKey = fallbackKey;

  PropVariantInit(&prop);
  hr = pDrive->lpVtbl->GetProperty(pDrive, &pKey, &prop);
  pDrive->lpVtbl->Release(pDrive);
  if (FAILED(hr)) return -1;

  switch (prop.vt) {
    case VT_UI4: case VT_I4: *outStatus = prop.ulVal; break;
    case VT_UI8:             *outStatus = (DWORD)prop.uhVal.QuadPart; break;
    default:
      PropVariantClear(&prop);
      return -1;
  }
  PropVariantClear(&prop);
  return 0;
}

static int isElevated(void) {
  HANDLE hToken = NULL;
  TOKEN_ELEVATION elev;
  DWORD cb = sizeof(elev);

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    return 0;
  if (!GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cb)) {
    CloseHandle(hToken);
    return 0;
  }
  CloseHandle(hToken);
  return elev.TokenIsElevated != 0;
}

static wchar_t *wnew(size_t nWchars) {
  wchar_t *p = (wchar_t *)malloc(nWchars * sizeof(wchar_t));
  ENFORCE(p != NULL, L"Out of memory");
  return p;
}

static wchar_t *wdup(const wchar_t *s) {
  wchar_t *p = wnew(wcslen(s) + 1);
  wcscpy(p, s);
  return p;
}

static void absolutePath(const wchar_t *rel, wchar_t *buf, size_t cch) {
  DWORD n = GetCurrentDirectoryW((DWORD)cch, buf);
  ENFORCE(n > 0 && n < cch, L"GetCurrentDirectoryW failed");
  if (buf[wcslen(buf) - 1] != L'\\') wcscat(buf, L"\\");
  wcscat(buf, rel);
}

static void driveName(const wchar_t *path, wchar_t *buf, size_t cch) {
  const wchar_t *colon = wcschr(path, L':');
  size_t len;
  ENFORCE(colon != NULL && colon != path, L"Path has no drive name");
  len = (size_t)(colon - path) + 1;
  ENFORCE(len < cch, L"Buffer too small");
  wcsncpy(buf, path, len);
  buf[len] = 0;
}

static void printHex16(const BYTE *p) {
  int i;
  for (i = 0; i < 16; i++)
    wprintf(L"%02X ", p[i]);
}

static void fillRandom(BYTE *buf, size_t len) {
  size_t i;
  for (i = 0; i < len; i++)
    buf[i] = (BYTE)(rand() & 0xFF);
}

static int endsWith(const wchar_t *s, const wchar_t *suffix) {
  size_t ls = wcslen(s), lf = wcslen(suffix);
  if (lf > ls) return 0;
  return wcscmp(s + ls - lf, suffix) == 0;
}

static int allBytesEq(const BYTE *p, size_t len, BYTE v) {
  size_t i;
  for (i = 0; i < len; i++)
    if (p[i] != v) return 0;
  return 1;
}

static void readln(void) {
  wchar_t line[256];
  fgetws(line, 256, stdin);
}

/* ---------- чтение/запись ---------- */

static BYTE *readBufferFromDisk(const wchar_t *ntDrivePath, ULONGLONG offset, size_t dataSize) {
  HANDLE hDriveRead;
  LARGE_INTEGER uliOffset;
  DWORD dwHigh;
  BYTE *readBuffer;
  DWORD dwNumberOfBytesRead;

  wprintf(L"  Opening %s...\n", ntDrivePath);
  hDriveRead = CreateFileW(ntDrivePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
  WENFORCE(hDriveRead != INVALID_HANDLE_VALUE, L"CreateFileW failed");

  wprintf(L"  Seeking to position %I64u...\n", offset);
  uliOffset.QuadPart = (LONGLONG)offset;
  dwHigh = (DWORD)uliOffset.HighPart;
  WENFORCE(SetFilePointer(hDriveRead, uliOffset.LowPart, &dwHigh, FILE_BEGIN) != INVALID_SET_FILE_POINTER, L"SetFilePointer failed");

  wprintf(L"  Reading %u bytes...\n", (unsigned)dataSize);
  readBuffer = (BYTE *)malloc(dataSize);
  ENFORCE(readBuffer != NULL, L"Out of memory");
  dwNumberOfBytesRead = 0;
  WENFORCE(ReadFile(hDriveRead, readBuffer, (DWORD)dataSize, &dwNumberOfBytesRead, NULL), L"ReadFile failed");
  if (dwNumberOfBytesRead != dataSize) diefmt(L"Read only %lu out of %u bytes", dwNumberOfBytesRead, (unsigned)dataSize);

  wprintf(L"  First 16 bytes: ");
  printHex16(readBuffer);
  wprintf(L"...\n");

  WENFORCE(CloseHandle(hDriveRead), L"CloseHandle failed");
  return readBuffer;
}

static void flushDiskBuffers(const wchar_t *ntDrivePath) {
  HANDLE hDrive;

  wprintf(L"Flushing buffers on %s...\n", ntDrivePath);
  wprintf(L"  Opening %s...\n", ntDrivePath);
  hDrive = CreateFileW(ntDrivePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  WENFORCE(hDrive != INVALID_HANDLE_VALUE, L"CreateFileW failed");

  wprintf(L"  Flushing buffers...\n");
  WENFORCE(FlushFileBuffers(hDrive), L"FlushFileBuffers failed");
  WENFORCE(CloseHandle(hDrive), L"CloseHandle failed");
}

static STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR detectSectorSize(const wchar_t *devName) {
  HANDLE hFile;
  STORAGE_PROPERTY_QUERY query;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR result;
  DWORD dwBytes;

  wprintf(L"  Obtaining sector size on %s...\n", devName);
  wprintf(L"    Opening %s...\n", devName);
  hFile = CreateFileW(devName, STANDARD_RIGHTS_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  WENFORCE(hFile != INVALID_HANDLE_VALUE, L"CreateFileW failed");

  query.QueryType = PropertyStandardQuery;
  query.PropertyId = StorageAccessAlignmentProperty;

  wprintf(L"    Querying storage alignment property...\n");
  memset(&result, 0, sizeof(result));
  WENFORCE(DeviceIoControl(hFile, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &result, sizeof(result), &dwBytes, NULL), L"DeviceIoControl(IOCTL_STORAGE_QUERY_PROPERTY) failed");

  wprintf(L"      BytesPerCacheLine             = %lu\n", result.BytesPerCacheLine);
  wprintf(L"      BytesOffsetForCacheAlignment  = %lu\n", result.BytesOffsetForCacheAlignment);
  wprintf(L"      BytesPerLogicalSector         = %lu\n", result.BytesPerLogicalSector);
  wprintf(L"      BytesPerPhysicalSector        = %lu\n", result.BytesPerPhysicalSector);
  wprintf(L"      BytesOffsetForSectorAlignment = %lu\n", result.BytesOffsetForSectorAlignment);

  WENFORCE(CloseHandle(hFile), L"CloseHandle failed");
  return result;
}

static void writeBuf(HANDLE hFile, const BYTE *data, size_t len) {
  DWORD dwNumberOfBytesWritten = 0;
  WENFORCE(WriteFile(hFile, data, (DWORD)len, &dwNumberOfBytesWritten, NULL), L"WriteFile failed");
  if (len != dwNumberOfBytesWritten)
    diefmt(L"Wrote only %lu out of %u bytes", dwNumberOfBytesWritten, (unsigned)len);
}

/* ---------- сохранение/загрузка данных (JSON, UTF-8/ASCII) ---------- */

static void jsonWriteString(FILE *f, const wchar_t *s) {
  const wchar_t *p;
  for (p = s; *p; p++) {
    wchar_t c = *p;
    switch (c) {
      case L'"':  fputs("\\\"", f); break;
      case L'\\': fputs("\\\\", f); break;
      case L'\n': fputs("\\n", f);  break;
      case L'\r': fputs("\\r", f);  break;
      case L'\t': fputs("\\t", f);  break;
      default:
        if (c >= 0x10000) {
          unsigned long u = (unsigned long)c - 0x10000;
          fprintf(f, "\\u%04X\\u%04X", (unsigned)(0xD800 + (u >> 10)), (unsigned)(0xDC00 + (u & 0x3FF)));
        } else if (c < 0x20 || c > 0x7E)
          fprintf(f, "\\u%04X", (unsigned)c);
        else
          fputc((int)(char)c, f);
    }
  }
}

static void saveJson(const wchar_t *path, const wchar_t *ntDrivePath,
  ULONGLONG offset, const BYTE *rnd, size_t rndSize, TrimReadBehavior behavior) {
  FILE *f;
  size_t i;

  f = _wfopen(path, L"wb");
  ENFORCE(f != NULL, L"Cannot open save file for writing");
  fputs("{\"ntDrivePath\":\"", f);
  jsonWriteString(f, ntDrivePath);
  fprintf(f, "\",\"offset\":%I64u,\"rndBuffer\":\"", offset);
  for (i = 0; i < rndSize; i++)
    fprintf(f, "%02X", rnd[i]);
  fprintf(f, "\",\"behavior\":%d}\n", (int)behavior);
  fclose(f);
}

static unsigned hexDigit(char c) {
  if (c >= '0' && c <= '9') return (unsigned)(c - '0');
  if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
  return 0xFFFFu;
}

static const char *skipWs(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '{' || *p == ',')
    p++;
  return p;
}

static const char *expectKey(const char *p, const char *key) {
  size_t klen = strlen(key);
  p = skipWs(p);
  if (*p != '"') die(L"Corrupt continuation data file");
  p++;
  if (strncmp(p, key, klen) != 0) die(L"Corrupt continuation data file");
  p += klen;
  if (*p != '"') die(L"Corrupt continuation data file");
  p++;
  p = skipWs(p);
  if (*p != ':') die(L"Corrupt continuation data file");
  return skipWs(p + 1);
}

static const char *parseStringValue(const char *p, wchar_t **out) {
  wchar_t *w;
  size_t n, cap;

  p = skipWs(p);
  if (*p != '"') die(L"Corrupt continuation data file");
  p++;
  cap = strlen(p) + 2;
  w = wnew(cap);
  n = 0;

  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      switch (*p) {
        case '"':  w[n++] = L'"';  break;
        case '\\': w[n++] = L'\\'; break;
        case '/':  w[n++] = L'/';  break;
        case 'b':  w[n++] = L'\b'; break;
        case 'f':  w[n++] = L'\f'; break;
        case 'n':  w[n++] = L'\n'; break;
        case 'r':  w[n++] = L'\r'; break;
        case 't':  w[n++] = L'\t'; break;
        case 'u':
        {
          unsigned long code, lo;
          if (hexDigit(p[1]) > 0xF || hexDigit(p[2]) > 0xF ||
            hexDigit(p[3]) > 0xF || hexDigit(p[4]) > 0xF)
            die(L"Corrupt continuation data file");
          code = (hexDigit(p[1]) << 12) | (hexDigit(p[2]) << 8) |
            (hexDigit(p[3]) << 4) | hexDigit(p[4]);
          p += 4;
          /* суррогатная пара */
          if (code >= 0xD800 && code <= 0xDBFF &&
            p[1] == '\\' && p[2] == 'u' &&
            hexDigit(p[3]) <= 0xF && hexDigit(p[4]) <= 0xF &&
            hexDigit(p[5]) <= 0xF && hexDigit(p[6]) <= 0xF) {
            lo = (hexDigit(p[3]) << 12) | (hexDigit(p[4]) << 8) |
              (hexDigit(p[5]) << 4) | hexDigit(p[6]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            }
          }
          if (code >= 0x10000) {
            unsigned long u = code - 0x10000;
            w[n++] = (wchar_t)(0xD800 + (u >> 10));
            w[n++] = (wchar_t)(0xDC00 + (u & 0x3FF));
          } else
            w[n++] = (wchar_t)code;
          break;
        }
        default:
          die(L"Corrupt continuation data file");
      }
      p++;
    } else if ((*p & 0x80) == 0) {
      w[n++] = (wchar_t)(BYTE)*p;
      p++;
    } else {
      /* декодирование UTF-8 (на всякий случай) */
      unsigned long code = 0;
      int cont, nextra;
      unsigned char c0 = (unsigned char)*p;
      if ((c0 & 0xE0) == 0xC0) { nextra = 1; code = c0 & 0x1F; } else if ((c0 & 0xF0) == 0xE0) { nextra = 2; code = c0 & 0x0F; } else if ((c0 & 0xF8) == 0xF0) { nextra = 3; code = c0 & 0x07; } else
        die(L"Corrupt continuation data file");
      for (cont = 0; cont < nextra; cont++) {
        p++;
        if (((unsigned char)*p & 0xC0) != 0x80)
          die(L"Corrupt continuation data file");
        code = (code << 6) | ((unsigned char)*p & 0x3F);
      }
      p++;
      if (code >= 0x10000) {
        unsigned long u = code - 0x10000;
        w[n++] = (wchar_t)(0xD800 + (u >> 10));
        w[n++] = (wchar_t)(0xDC00 + (u & 0x3FF));
      } else
        w[n++] = (wchar_t)code;
    }
  }
  w[n] = 0;
  *out = w;
  return p + 1;
}

static const char *parseHexValue(const char *p, BYTE **out, size_t *outLen) {
  size_t len, i;
  BYTE *buf;

  p = skipWs(p);
  if (*p != '"') die(L"Corrupt continuation data file");
  p++;
  len = 0;
  while (p[len] && p[len] != '"')
    len++;
  if (len == 0 || (len % 2) != 0)
    die(L"Corrupt continuation data file");
  buf = (BYTE *)malloc(len / 2);
  ENFORCE(buf != NULL, L"Out of memory");
  for (i = 0; i < len / 2; i++) {
    unsigned hi = hexDigit(p[2 * i]);
    unsigned lo = hexDigit(p[2 * i + 1]);
    if (hi > 0xF || lo > 0xF)
      die(L"Corrupt continuation data file");
    buf[i] = (BYTE)((hi << 4) | lo);
  }
  *out = buf;
  *outLen = len / 2;
  return p + len + 1;
}

static void loadSaveData(const wchar_t *path, SaveData *sd) {
  FILE *f;
  long fsize;
  char *text;
  size_t nread;
  const char *p;
  char *end;

  f = _wfopen(path, L"rb");
  ENFORCE(f != NULL, L"Cannot open save file");
  fseek(f, 0, SEEK_END);
  fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  ENFORCE(fsize > 0, L"Corrupt continuation data file");
  text = (char *)malloc((size_t)fsize + 1);
  ENFORCE(text != NULL, L"Out of memory");
  nread = fread(text, 1, (size_t)fsize, f);
  text[nread] = 0;
  fclose(f);

  p = expectKey(text, "ntDrivePath");
  p = parseStringValue(p, &sd->ntDrivePath);
  p = expectKey(p, "offset");
  sd->offset = _strtoui64(p, &end, 10);
  if (end == p)
    die(L"Corrupt continuation data file");
  p = expectKey(end, "rndBuffer");
  p = parseHexValue(p, &sd->rndBuffer, &sd->rndSize);
  sd->behavior = TRB_UNKNOWN;
  p = skipWs(p);
  if (*p == ',') {
    p = expectKey(p + 1, "behavior");
    sd->behavior = (TrimReadBehavior)strtol(p, &end, 10);
    if (end == p)
      die(L"Corrupt continuation data file");
  }
  free(text);
}

/* ---------- DRAT / RZAT / DLFEAT detection ----------
 * Minimal local definitions of the STORAGE_PROTOCOL_QUERY_PROPERTY
 * structures (documented for Windows 8+). Kept independent of the SDK
 * version so this builds with MSVC 2010 / Windows SDK 7.0a. */

#define TC_PROP_PROTOCOL_SPECIFIC  50  /* StorageDeviceProtocolSpecificProperty */
#define TC_PROTO_ATA               2
#define TC_PROTO_NVME              3
#define TC_ATA_DATA_IDENTIFY       1
#define TC_NVME_DATA_IDENTIFY      1

#ifndef BusTypeNvme
#define BusTypeNvme 17
#endif

typedef struct {
  DWORD protocolType;
  DWORD dataType;
  DWORD requestValue;
  DWORD requestSubValue;
  DWORD dataOffset;
  DWORD dataLength;
  DWORD fixedReturn;
  DWORD reserved[3];
} TC_PROTO_DATA;

typedef struct {
  DWORD version;
  DWORD size;
  TC_PROTO_DATA protoData;
} TC_PROTO_DESC;

typedef struct {  /* mirrors STORAGE_PROPERTY_QUERY */
  DWORD propertyId;
  DWORD queryType;
  TC_PROTO_DATA protoData;
} TC_QUERY;

static BOOL queryProtocolData(HANDLE hDevice, const TC_QUERY *query,
  BYTE *outBuf, DWORD outSize, const BYTE **data) {
  TC_PROTO_DESC *desc;
  DWORD bytes = 0;

  *data = NULL;
  if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
      (void *)query, sizeof(*query), outBuf, outSize, &bytes, NULL))
    return FALSE;
  if (bytes < sizeof(TC_PROTO_DESC))
    return FALSE;
  desc = (TC_PROTO_DESC *)outBuf;
  if (desc->version != 1 || bytes < (DWORD)sizeof(TC_PROTO_DESC) + query->protoData.dataLength)
    return FALSE;
  /* dataOffset is relative to the start of TC_PROTO_DATA */
  *data = (const BYTE *)&desc->protoData + desc->protoData.dataOffset;
  return TRUE;
}

static DWORD getBusType(HANDLE hDevice) {
  STORAGE_PROPERTY_QUERY q;
  BYTE out[512];
  STORAGE_DEVICE_DESCRIPTOR *d;
  DWORD bytes = 0;

  q.PropertyId = StorageDeviceProperty;
  q.QueryType = PropertyStandardQuery;
  if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
      out, sizeof(out), &bytes, NULL))
    return 0;
  if (bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR))
    return 0;
  d = (STORAGE_DEVICE_DESCRIPTOR *)out;
  return d->BusType;
}

static TrimReadBehavior queryAtaBehavior(HANDLE hDevice) {
  BYTE outBuf[1024];
  TC_QUERY query;
  const BYTE *data;
  const WORD *id;

  ZeroMemory(&query, sizeof(query));
  query.propertyId = TC_PROP_PROTOCOL_SPECIFIC;
  query.protoData.protocolType = TC_PROTO_ATA;
  query.protoData.dataType = TC_ATA_DATA_IDENTIFY;
  query.protoData.dataOffset = sizeof(TC_PROTO_DATA);
  query.protoData.dataLength = 512;
  if (!queryProtocolData(hDevice, &query, outBuf, sizeof(outBuf), &data))
    return TRB_UNKNOWN;
  id = (const WORD *)data;
  if (!(id[169] & 0x0001))  /* word 169, bit 0: DATA SET MANAGEMENT (TRIM) */
    return TRB_UNKNOWN;
  if (id[69] & 0x4000)      /* word 69, bit 14: DRAT */
    return (id[69] & 0x0020) ? TRB_RZAT : TRB_DRAT;  /* word 69, bit 5: RZAT */
  return TRB_NONDET;
}

static TrimReadBehavior queryNvmeBehavior(HANDLE hDevice) {
  BYTE outBuf[8192];
  TC_QUERY query;
  const BYTE *data;
  DWORD candidates[3];
  int i;

  /* NSID candidates: broadcast, then 0 and 1 - stornvme resolves the
   * namespace of the device handle for these in practice */
  candidates[0] = 0xFFFFFFFF;
  candidates[1] = 0;
  candidates[2] = 1;
  for (i = 0; i < 3; i++) {
    ZeroMemory(&query, sizeof(query));
    query.propertyId = TC_PROP_PROTOCOL_SPECIFIC;
    query.protoData.protocolType = TC_PROTO_NVME;
    query.protoData.dataType = TC_NVME_DATA_IDENTIFY;
    query.protoData.requestValue = 1;              /* CNS = 1: Identify Namespace */
    query.protoData.requestSubValue = candidates[i];
    query.protoData.dataOffset = sizeof(TC_PROTO_DATA);
    query.protoData.dataLength = 4096;
    if (!queryProtocolData(hDevice, &query, outBuf, sizeof(outBuf), &data))
      continue;
    switch (data[33] & 0x07) {  /* DLFEAT (byte 33), bits 2:0 */
      case 0x01: return TRB_RZAT;
      case 0x02: return TRB_FFAT;
      case 0x00: return TRB_NONDET;
      default:   return TRB_DRAT;
    }
  }
  return TRB_UNKNOWN;
}

static TrimReadBehavior queryTrimReadBehavior(HANDLE hDevice) {
  DWORD bus;
  TrimReadBehavior b;

  bus = getBusType(hDevice);
  if (bus == BusTypeNvme)
    return queryNvmeBehavior(hDevice);
  if (bus == BusTypeAta || bus == BusTypeSata)
    return queryAtaBehavior(hDevice);
  /* Unknown bus (USB bridge etc.) - try both protocols */
  b = queryAtaBehavior(hDevice);
  if (b == TRB_UNKNOWN)
    b = queryNvmeBehavior(hDevice);
  return b;
}

static const wchar_t *behaviorName(TrimReadBehavior b) {
  switch (b) {
    case TRB_NONDET: return L"non-determinate read after TRIM";
    case TRB_DRAT:   return L"DRAT (deterministic, not zeroed)";
    case TRB_RZAT:   return L"RZAT (zeros after TRIM)";
    case TRB_FFAT:   return L"0xFF after TRIM (NVMe)";
    default:         return L"unknown";
  }
}

/* ---------- create ---------- */

static void create(void) {
  wchar_t pathBuf[4096];
  wchar_t drive[16];
  wchar_t drivePathBS[20];
  wchar_t ntDrivePath[20];
  wchar_t *paths[4];
  DWORD kinds[4];
  DWORD dwSectorsPerCluster, dwBytesPerSector, dwNumberOfFreeClusters, dwTotalNumberOfClusters;
  size_t dataSize;
  ULONGLONG clusterBytes, freeBytes;
  BYTE *rndBuffer;
  BYTE *garbageData;
  HANDLE hFile;
  BY_HANDLE_FILE_INFORMATION fileInformation;
  LONGLONG dataStartVCN, dataEndVCN;
  ULONGLONG offset;
  TrimReadBehavior behavior;
  HANDLE hProbe;
  int n, k;

  wprintf(L"USAGE: Place this program file on the same drive\n");
  wprintf(L"you'd like to test TRIM on, and run it.\n\n");
  absolutePath(DATAFILENAME, pathBuf, 4096);
  driveName(pathBuf, drive, 16);
  wprintf(L"Press Enter to test drive %s...\n", drive);
  readln();

  wcscpy(drivePathBS, drive);
  wcscat(drivePathBS, L"\\");
  wprintf(L"Querying %s disk space and sector size information...\n", drivePathBS);
  WENFORCE(GetDiskFreeSpaceW(drivePathBS, &dwSectorsPerCluster, &dwBytesPerSector, &dwNumberOfFreeClusters, &dwTotalNumberOfClusters), L"GetDiskFreeSpaceW failed");
  wprintf(L"  %s has %lu bytes per sector, and %lu sectors per cluster.\n", drivePathBS, dwBytesPerSector, dwSectorsPerCluster);
  wprintf(L"  %lu out of %lu clusters are free.\n", dwNumberOfFreeClusters, dwTotalNumberOfClusters);

  clusterBytes = (ULONGLONG)dwBytesPerSector * dwSectorsPerCluster;
  dataSize = 16 * 1024;
  if (clusterBytes > dataSize)
    dataSize = (size_t)clusterBytes;
  ENFORCE(dataSize % (size_t)clusterBytes == 0, L"Unsupported cluster size, please report this.");
  freeBytes = (ULONGLONG)dwNumberOfFreeClusters * clusterBytes;
  ENFORCE(freeBytes > (ULONGLONG)dataSize + (ULONGLONG)PADDINGSIZE_MB * MB * 2, L"Disk space is too low!");

  wprintf(L"Generating random target data block (%u bytes)...\n", (unsigned)dataSize);
  rndBuffer = (BYTE *)_aligned_malloc(dataSize, 4096);
  ENFORCE(rndBuffer != NULL, L"Out of memory");
  fillRandom(rndBuffer, dataSize);
  wprintf(L"  First 16 bytes: ");
  printHex16(rndBuffer);
  wprintf(L"...\n");

  wprintf(L"Creating %s...\n", DATAFILENAME);
  hFile = CreateFileW(DATAFILENAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING, NULL);
  WENFORCE(hFile != INVALID_HANDLE_VALUE, L"CreateFileW failed");
  g_hTestFile = hFile;      /* при аварийном выходе die() закроет и удалит */
  g_hTestFileOpen = 1;

  WENFORCE(GetFileInformationByHandle(hFile, &fileInformation), L"GetFileInformationByHandle failed");
  ENFORCE((fileInformation.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) == 0, L"TrimCheck cannot reliably work on a compressed filesystem. Please rerun from an uncompressed directory.");

  wcscpy(ntDrivePath, L"\\\\.\\");
  wcscat(ntDrivePath, drive);

  wprintf(L"Querying file final paths...\n");

  kinds[0] = VOLUME_NAME_DOS;
  kinds[1] = VOLUME_NAME_GUID;
  kinds[2] = VOLUME_NAME_NT;
  kinds[3] = VOLUME_NAME_NONE;

  for (k = 0; k < 4; k++) {
    DWORD len = GetFinalPathNameByHandleW(hFile, pathBuf, 4096, kinds[k] | FILE_NAME_NORMALIZED);
    WENFORCE(len > 0 && len < 4096, L"GetFinalPathNameByHandleW failed");
    pathBuf[len] = 0;
    paths[k] = wdup(pathBuf);
  }

  wprintf(L"  DOS  : %s\n", paths[0]);
  wprintf(L"  GUID : %s\n", paths[1]);
  wprintf(L"  NT   : %s\n", paths[2]);
  wprintf(L"  NONE : %s\n", paths[3]);

  ENFORCE(endsWith(paths[0], paths[3]), L"DOS  path does not end with NONE path");
  ENFORCE(endsWith(paths[1], paths[3]), L"GUID path does not end with NONE path");
  ENFORCE(endsWith(paths[2], paths[3]), L"NT   path does not end with NONE path");

  /* icmp: сравнение "C:" из ntDrivePath с "C:" из DOS-пути без \\?\ и без NONE-суффикса */
  {
    size_t dosLen = wcslen(paths[0]);
    size_t noneLen = wcslen(paths[3]);
    static wchar_t tmp[4096];
    ENFORCE(dosLen - 4 > noneLen, L"Unexpected DOS path format");
    wcsncpy(tmp, paths[0] + 4, dosLen - 4 - noneLen);
    tmp[dosLen - 4 - noneLen] = 0;
    ENFORCE(_wcsicmp(ntDrivePath + 4, tmp) == 0, L"Current directory seems to be located under a reparse point\nwhich points to another drive. Try placing the program file in the\nroot directory of the drive you wish to test.");
  }

  for (k = 0; k < 4; k++)
    free(paths[k]);

  garbageData = (BYTE *)_aligned_malloc(MB, 4096);
  ENFORCE(garbageData != NULL, L"Out of memory");

  wprintf(L"Writing padding (%d bytes)...\n", PADDINGSIZE_MB * MB);
  for (n = 0; n < PADDINGSIZE_MB; n++) {
    fillRandom(garbageData, MB);
    writeBuf(hFile, garbageData, MB);
  }

  wprintf(L"Writing data (%u bytes)...\n", (unsigned)dataSize);
  writeBuf(hFile, rndBuffer, dataSize);

  wprintf(L"Writing padding (%d bytes)...\n", PADDINGSIZE_MB * MB);
  for (n = 0; n < PADDINGSIZE_MB; n++) {
    fillRandom(garbageData, MB);
    writeBuf(hFile, garbageData, MB);
  }

  wprintf(L"Flushing file...\n");
  WENFORCE(FlushFileBuffers(hFile), L"FlushFileBuffers failed");

  wprintf(L"Checking file size...\n");
  ENFORCE(GetFileSize(hFile, NULL) == (DWORD)(PADDINGSIZE_MB * MB * 2 + dataSize), L"Unexpected file size");

  dataStartVCN = (LONGLONG)((ULONGLONG)PADDINGSIZE_MB * MB / clusterBytes);
  dataEndVCN = dataStartVCN + (LONGLONG)((ULONGLONG)dataSize / clusterBytes);
  wprintf(L"  Data is located at Virtual Cluster Numbers %I64d-%I64d within file.\n", dataStartVCN, dataEndVCN - 1);
  wprintf(L"Querying file physical location...\n");
  {
    STARTING_VCN_INPUT_BUFFER svib;
    RETRIEVAL_POINTERS_BUFFER *prpb;
    BYTE *rpbBuf;
    DWORD c;
    LONGLONG prevVcn;
    DWORD n2;

    svib.StartingVcn.QuadPart = 0;
    rpbBuf = (BYTE *)_aligned_malloc(64 * 1024, 4096);
    ENFORCE(rpbBuf != NULL, L"Out of memory");
    prpb = (RETRIEVAL_POINTERS_BUFFER *)rpbBuf;

    WENFORCE(DeviceIoControl(hFile, FSCTL_GET_RETRIEVAL_POINTERS, &svib, sizeof(svib), prpb, 64 * 1024, &c, NULL), L"DeviceIoControl(FSCTL_GET_RETRIEVAL_POINTERS) failed");

    wprintf(L"  %s has %lu extent%s:\n", DATAFILENAME, prpb->ExtentCount, prpb->ExtentCount == 1?L"":L"s");
    offset = 0;
    prevVcn = prpb->StartingVcn.QuadPart;
    for (n2 = 0; n2 < prpb->ExtentCount; n2++) {
      LONGLONG startVCN = prevVcn;
      LONGLONG endVCN = prpb->Extents[n2].NextVcn.QuadPart;

      if (startVCN == endVCN - 1)
        wprintf(L"    Extent %lu: Virtual cluster %I64d is located at LCN %I64d\n", n2, startVCN, prpb->Extents[n2].Lcn.QuadPart);
      else
        wprintf(L"    Extent %lu: Virtual clusters %I64d-%I64d are located at LCN %I64d\n", n2, startVCN, endVCN - 1, prpb->Extents[n2].Lcn.QuadPart);

      if (startVCN <= dataStartVCN && endVCN >= dataEndVCN) {
        LONGLONG dataLCN;
        wprintf(L"      (this is the extent containing our data)\n");
        dataLCN = prpb->Extents[n2].Lcn.QuadPart + (dataStartVCN - startVCN);
        offset = (ULONGLONG)dataLCN * clusterBytes;
      }
      prevVcn = endVCN;
    }

    for (n2 = 0; n2 < prpb->ExtentCount; n2++)
      ENFORCE(prpb->Extents[n2].Lcn.QuadPart > 0, L"The Logical Cluster Number of extent is not set. " L"Perhaps the file is compressed?");
    ENFORCE(offset != 0, L"Could not find the extent of the data part of file.");

    _aligned_free(rpbBuf);
  }

  wprintf(L"Closing file.\n");
  WENFORCE(CloseHandle(hFile), L"CloseHandle failed");
  g_hTestFileOpen = 0;

  wprintf(L"Saving continuation data to %s...\n", SAVEFILENAME);
  wprintf(L"Querying drive TRIM read behavior (DRAT/RZAT/DLFEAT)...\n");
  behavior = TRB_UNKNOWN;
  hProbe = CreateFileW(ntDrivePath, GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (hProbe != INVALID_HANDLE_VALUE) {
    behavior = queryTrimReadBehavior(hProbe);
    CloseHandle(hProbe);
  } else {
    wprintf(L"  (could not open drive for capability query, skipping)\n");
  }
  wprintf(L"  Drive reports: %s\n", behaviorName(behavior));

  saveJson(SAVEFILENAME, ntDrivePath, offset, rndBuffer, dataSize, behavior);

  flushDiskBuffers(ntDrivePath);

  wprintf(L"Deleting file...\n");
  WENFORCE(DeleteFileW(DATAFILENAME), L"DeleteFile failed");

  flushDiskBuffers(ntDrivePath);

  _aligned_free(garbageData);
  _aligned_free(rndBuffer);

  wprintf(L"\n");
  wprintf(L"Test file created and deleted, and continuation data saved.\n");
  wprintf(L"Do what needs to be done to activate the SSD's TRIM functionality,\n");
  wprintf(L"and run this program again.\n");
  wprintf(L"Usually, you just need to wait a bit (around 20 seconds).\n");
  wprintf(L"Sometimes, a reboot is necessary.\n");
}

/* ---------- verify ---------- */

static void verify(void) {
  SaveData sd;
  size_t dataSize;
  BYTE *readBuffer;

  /* аналог scope(failure) из D: подсказка при ошибке */
  wcscpy(g_failureHint, L"\nAn error has occurred during verification.\n" L"To start from scratch, delete " SAVEFILENAME L".\n\n");

  wprintf(L"Loading continuation data from %s...\n", SAVEFILENAME);
  loadSaveData(SAVEFILENAME, &sd);
  wprintf(L"  Drive path   :  %s\n", sd.ntDrivePath);
  wprintf(L"  Offset       :  %I64u\n", sd.offset);
  wprintf(L"  Random data  : ");
  printHex16(sd.rndBuffer);
  wprintf(L"...\n\n");

  dataSize = sd.rndSize;

  wprintf(L"Reading raw volume data...\n");
  readBuffer = readBufferFromDisk(sd.ntDrivePath, sd.offset, dataSize);
  g_failureHint[0] = 0;

  if (allBytesEq(readBuffer, dataSize, 0x00) ||
      allBytesEq(readBuffer, dataSize, 0xFF)) {
    wprintf(L"Data is empty (filled with 0x%02X bytes).\n\n", readBuffer[0]);
    wprintColor(COLOR_GREEN, L"CONCLUSION: TRIM appears to be WORKING!\n");
    DeleteFileW(SAVEFILENAME);
  } else if (memcmp(readBuffer, sd.rndBuffer, dataSize) == 0) {
    wprintf(L"Data unchanged.\n\n");
    if (sd.behavior == TRB_RZAT || sd.behavior == TRB_FFAT) {
      wprintColor(COLOR_RED, L"CONCLUSION: TRIM appears to be NOT WORKING.\n\n");
      wprintf(L"This drive reports that reads after TRIM return zeros/0xFF,\n");
      wprintf(L"yet our data came back intact. If you just ran the test,\n");
      wprintf(L"wait around 20 seconds and re-run; also check that TRIM is\n");
      wprintf(L"enabled (fsutil behavior query DisableDeleteNotify) and that\n");
      wprintf(L"the drive is not behind a USB bridge that does not pass TRIM.\n\n");
    } else if (sd.behavior == TRB_DRAT) {
      wprintColor(COLOR_RED, L"CONCLUSION: INDETERMINATE for this drive model.\n\n");
      wprintf(L"This drive reports DRAT (deterministic read after TRIM)\n");
      wprintf(L"but NOT RZAT (zeros after TRIM). Such drives may legitimately\n");
      wprintf(L"return the old data after TRIM, so trimcheck cannot\n");
      wprintf(L"distinguish this case from \"TRIM not working\".\n");
      wprintf(L"For a definitive answer, test this drive from Linux\n");
      wprintf(L"(blkdiscard on a temporary file, then read it back),\n");
      wprintf(L"or use a kernel-level TRIM monitor.\n\n");
    } else {
      wprintColor(COLOR_RED, L"CONCLUSION: TRIM appears to be NOT WORKING\n(or has not kicked in yet).\n\n");
      if (sd.behavior == TRB_UNKNOWN)
        wprintf(L"Note: the drive's read-after-TRIM behavior could not be\nqueried (common behind USB bridges, or on Windows older than\nWindows 8), so the classic caveat applies: a drive without\nRZAT support may return the old data even when TRIM works.\n\n");
    }
    wprintf(L"You can re-run this program to test again with the same data block,\n");
    wprintf(L"or delete %s to create a new test file.\n", SAVEFILENAME);
  } else {
    wprintf(L"Data is neither unchanged nor empty.\n");
    wprintf(L"Possible cause: another program saved data to disk,\n");
    wprintf(L"overwriting the sector containing our test data.\n\n");
    wprintColor(COLOR_RED,L"CONCLUSION: INDETERMINATE.\n");
    wprintf(L"Re-run this program and wait less before verifying / try to\nminimize writes to drive %s.\n", sd.ntDrivePath + wcslen(sd.ntDrivePath) - 2);
    DeleteFileW(SAVEFILENAME);
  }

  free(readBuffer);
  free(sd.rndBuffer);
  free(sd.ntDrivePath);
}

static void run(void) {
  wchar_t pathBuf[4096];
  wchar_t drive[16];
  HRESULT hrCom;
  DWORD bitLockerStatus;

  wprintf(L"TRIM check v0.7 - Written by Vladimir Panteleev\n");
  wprintf(L"https://github.com/CyberShadow/trimcheck\n\n");

  if (!isElevated()) {
    wprintColor(COLOR_RED,L"Error: This program must be run as Administrator.\n");
    wprintColor(COLOR_RED,L"Right-click the executable and select \"Run as administrator\".\n\n");
    return;
  }

  hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (SUCCEEDED(hrCom)) {
    absolutePath(DATAFILENAME, pathBuf, 4096);
    driveName(pathBuf, drive, 16);
    if (getBitLockerStatus(drive[0], &bitLockerStatus) == 0) {
      if (bitLockerStatus == 1 || bitLockerStatus == 3 || bitLockerStatus == 5) {
        wprintf(L"Drive %s is encrypted with BitLocker.\n", drive);
        wprintf(L"The test reads raw volume data, which is meaningless on an\n");
        wprintf(L"encrypted volume. Disable BitLocker (suspend is not enough -\n");
        wprintf(L"status 5 still means data is encrypted), then rerun this test.\n\n");
        return;
      } else
        wprintf(L"Drive %s is not encrypted with BitLocker.\n\n", drive);
    } else {
      wprintf(L"Could not query BitLocker status, continuing anyway.\n\n");
    }
    CoUninitialize();
  }

  if (GetFileAttributesW(SAVEFILENAME) == INVALID_FILE_ATTRIBUTES)
    create();
  else
    verify();
}

/* ---------- main ---------- */
int main(void) {
  srand((unsigned)time(NULL));

  if (setjmp(g_exitJmp) == 0)
    run();
  /* при setjmp != 0 сообщение об ошибке уже напечатано die() */

  wprintf(L"\nPress Enter to exit...\n");
  readln();
  return 0;
}
