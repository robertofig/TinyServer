#ifndef TINYSERVER_HTTP_H
//===========================================================================
// tinyserver-http.h
//
// Module for working with the HTTP protocol. Can be used in conjunction
// with the main TinyServer framework for servers that deal with HTTP to
// help parse request header and body, and create response header and
// cookies.
//
// Reading inbound data:
//   1. Recv incoming data.
//   2. Parse data with ParseHttpHeader().
//   3. If result is HttpParse_HeaderIncomplete, repeat from step 1 until
//      the result is HttpParse_OK, or HttpParse_BodyIncomplete.
//   4. If result is HttpParse_BodyIncomplete, the difference between
//      [.TotalSize] and [.Received] in ts_request_body object shows how
//      much else to recv.
//
// Sending outbound data:
//   1. Create a ts_response object, fill its [.StatusCode], [.Version] and
//      [.KeepAlive] members with the appropriate info.
//   2. If response sends cookies, craft the cookies in a separate memory
//      buffer and fill [.Cookies] and [.CookiesSize] members of ts_response
//      with the info. Cookies must end on blank /r/n line.
//   3. If response sends payload, write the payload in a separate memory
//      buffer and fill [.Payload] and [.PayloadSize] members of ts_response
//      with the info. [.PayloadType] must point to a zero-terminated array
//      with the payload MIME type.
//   4. Once ts_response is fully filled, prepare a memory buffer for the
//      response with at least 1KB of available size.
//   5. Call CraftHttpResponseHeader().
//   6. Send the response header buffer, cookies buffer, and payload buffer
//      in that exact order, if there are cookies and payload to be sent.
//===========================================================================
#define TINYSERVER_HTTP_H

#define MAX_NUM_HEADERS 255
#define IMF_DATE_LENGTH 30

global char* gServerName = ""; // Left blank to be set by server app.


//================================
// Request Header
//================================

typedef enum ts_http_parse
{
    HttpParse_OK,
    HttpParse_HeaderIncomplete,
    HttpParse_BodyIncomplete,
    HttpParse_HeaderInvalid,
    HttpParse_HeaderMalicious,
    HttpParse_TooManyHeaders
} ts_http_parse;

#define HttpVerb_Unknown 0
#define HttpVerb_Get     1
#define HttpVerb_Head    2
#define HttpVerb_Post    3
#define HttpVerb_Put     4
#define HttpVerb_Delete  5
#define HttpVerb_Connect 6
#define HttpVerb_Options 7
#define HttpVerb_Trace   8
#define HttpVerb_Patch   9

#define HttpVersion_Unknown 0
#define HttpVersion_09      1
#define HttpVersion_10      2
#define HttpVersion_11      3
#define HttpVersion_20      4

typedef struct ts_request
{
    char* Base;
    u16 HeaderSize;
    u8 NumHeaders;
    
    u8 Verb;
    u8 Version;
    u8 UriOffset;
    u16 PathSize;
    u16 QuerySize;
    
    u16 FirstHeaderOffset;
} ts_request;

typedef struct ts_request_body
{
    u8* Base;
    u64 Received;
    u64 TotalSize;
    
    char* ContentType;
    u16 ContentTypeSize;
} ts_request_body;

external ts_http_parse ParseHttpHeader(string InBuffer, ts_request* Request, ts_request_body* Body);

/* Parses an incoming request pointed at by [InBuffer]. The parsing is done on
 |  the [Request] and [Body] objects, which must be zeroed on first call. The
 |  request may not be complete, in which case the function parses however much
 |  it can and indicates that there is more data to be read. For further calls
 |  to the function, either the old [Request] and [Body] objects can be passed,
|  in which case the parsing resumes where it ended, or new (zeroed) objects,
|  which will start the parsin again from the top. The result is the same.
|--- Return: HttpParse_OK if completed successfully, HttpParse_HeaderIncomplete
|    if there's still more data to read, HttpParse_BodyIncomplete if there's
|    more body data to read, or one of the error codes if failure. */

external string GetHeaderByKey(ts_request* Request, char* TargetKey);

/* Given a fully parsed [Request], search for the value of the [TargetKey] header.
|  [TargetKey] must be a zero-terminated array.
|--- Return: string pointing to data, or empty string if header not found.*/

external string GetHeaderByIdx(ts_request* Request, usz TargetIdx);

/* Given a fully parsed [Request], search for the value of the header of count
|  [TargetIdx]. The index goes from 0..NumHeaders member in [Request].
|--- Return: string pointing to data, or empty string if index is beyond limit. */


//================================
// Request Body
//================================

typedef struct ts_form_field
{
    char* FieldName;
    char* Filename;
    char* Charset;
    u16 FieldNameSize; //fieldnamelen
    u16 FilenameSize; //filenamelen
    u16 CharsetSize; //charsetlen
    
    void* Data;
    u32 DataLen; //datalen
} ts_form_field;

typedef struct ts_multiform
{
    usz FieldCount;
    void* FirstField;
    void* LastField;
} ts_multiform;

external ts_multiform ParseFormData(ts_request_body Body);

/* Parses request body of content-type "multipart/form-data". [Body] must be
|  fully initialised, and the entire content of the body must have been read
|  to memory already. This function does not support partial parsing.
|--- Return: struct with form info, or empty struct if parsing failed. */

external ts_form_field GetFormFieldByName(ts_multiform Form, char* TargetName);

/* Given a fully parsed [Form], search for the info of the [TargetName] field.
|  [TargetName] must be a zero-terminated array.
|--- Return: struct with field info, or empty struct if field name not found. */

external ts_form_field GetFormFieldByIdx(ts_multiform Form, usz TargetIdx);

/* Given a fully parsed [Form], search for the info of the field of count.
|  [TargetIdx]. The index goes from 0..FieldCount member in [Form]..
|--- Return: struct with field info, or empty struct if index is beyond limit. */


//================================
// Response
//================================

typedef enum ts_cookie_attr
{
    CookieAttr_ExpDate        = 0x1,
    CookieAttr_MaxAge         = 0x2,
    CookieAttr_Domain         = 0x4,
    CookieAttr_Path           = 0x8,
    CookieAttr_Secure         = 0x10,
    CookieAttr_HttpOnly       = 0x20,
    CookieAttr_SameSiteStrict = 0x40,
    CookieAttr_SameSiteLax    = 0x80,
    CookieAttr_SameSiteNone   = 0x100
} ts_cookie_attr;

typedef struct ts_cookie
{
    string  Name;
    string Value;
    
    datetime ExpDate;
    u32 MaxAge;
    string Domain;
    string Path;
    
    u32 AttrFlags;
} ts_cookie;

typedef struct ts_response
{
    u16 HeaderSize;
    
    u16 StatusCode;
    u8 Version; // Same value parsed in ts_request.
    u8 KeepAlive;
    
    u16 CookiesSize;
	char* Cookies;
    
    char* Payload;
    usz PayloadSize;
	char* PayloadType;
} ts_response;

external void CraftHttpResponseHeader(ts_response* Response, string* OutHeader);

/* Creates HTTP response header based on [Response] data, into [OutHeader].
  |  [OutHeader] must have enough space allocated to accomodate the full
|  header (about 1KB is enough). If [Response] contains pointer to cookies
|  and/or payload, these are not written to OutHeader, but rather must
|  be sent separately.
|--- Return: nothing. */


#if !defined(TINYSERVER_STATIC_LINKING)
#include "tinyserver-http.c"
#endif

#endif //TINYSERVER_HTTP_H
