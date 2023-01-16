# TinyServer - Application

## Introduction
Tinyserver supports an application to be loaded during runtime, which takes over the processing in cases of PUT, POST, and DELETE requests, as well as GET and HEAD requests that specify a method path on the URI (as opposed to a file).

Entering the application can be done on the same thread that handled the request IO/parsing, or on a separate thread per request. This potentially slows the processing of the application, as it incurs in more dispute for resources and context switches, but ensures a high throughput in case the application does heavy processing. Choosing whether to spawn a new thread or not can be controlled on the `app-new-thread` parameter of the server conf file.

## Building the app
The application must be built into a dynamic library (dll on Windows, so on Linux), whose name must be specified on the `app-library-name` parameter of the conf file, and the path where the app resides on the `app-base-path` parameter. This path also becomes the working directory when entering the app, so any calls to the filesystem from within the app have this path as its base.

No dependencies are required when building the app, only the inclusion of a header file, located in the ./include/ folder in this repo. There are two files present, `tinyserver.h` for C apps, and `tinyserver.hpp` for C++ apps. They are identical except for the way to call functions, and app must include only one of these. They are standalone headers, not including anything else, so feel free to move them to whichever dev environment you may use for the app. Apps written on languages other than C/C++ can be built by creating a wrapper around these includes.

The server supports hot-reloading of the app library, so it can be updated or substituted at any moment without having to stop the server. It achieves that by creating a copy of the library and loading from it, then any time a request is about to enter the app the server compares the last write times of both files, and updates the copy in case of an update.

## API
All apps have a common entrypoint in the `ModuleMain` function, whose declaration is as follows:
```c
void ModuleMain(ts_http* Http);
```
If building on C++, this function must be preceded by `extern "C"` or any other method that prevents name-mangling. If this function's name is exported mangled, the server will not be able to load it.

The `ts_http` struct is the bread-and-butter of the API, it is from which the app gets the details of the HTTP request, and through which the app tells the server how to build the response.
```c
typedef struct
{
    const ts_http_verb Verb;         // HTTP request method.
    const ts_string Path;            // URI path.
    const ts_string Query;           // URI query.
    const size_t HeaderCount;        // Number of request header fields.
    void* const Entity;              // Raw request body.
    const size_t EntitySize;         // Size of the request body.
    const ts_mime_type EntityType;   // MIME type of the request body.
    
    // ...
} ts_http;
```

The `ts_http_verb` and `ts_mime_type` types are enums defined in the header file, and `ts_string` is a struct for non-null terminated strings, with the following definition:
```c
typedef struct
{
    char* Ptr;        // Pointer to the beginning of the string.
    size_t Size;      // Number of bytes on the string.
    size_t MaxSize;   // Size of the buffer that holds the string.
} ts_string;
```

Since `ts_string` objects are not null-terminated (or rather, not guaranteed to be), the Size parameter must be used to determine how much needs to be read. The MaxSize value will be 0 whenever the buffer is not expected to be written to, and will contain the full size of the allocated buffer when expected to.

Aside from these informations on the request, the `ts_http` struct brings several methods for accessing the data on the request and configuring the response. These methods are passed as function pointers from the server, and are accessed differently depending if the App is being built on C or C++. On C, the header creates global-scope functions with a TS prefix that work as stubs for the function pointers and take the `ts_http` object as first parameter, whereas on C++ each function pointer has a corresponding class method that calls the function pointer, implicitly passing the `this` pointer to it.

As an example, the method `int Add(int, int);` would be called like this on each version of the API:
```c++
// C version.
TSAdd(Http, 4, 6);

// C++ version.
Http->Add(4, 6);
```



## Methods
Here comes listed all methods and types they may use, showing both the declarations for the C and C++ versions.

---
#### GetHeaderByKey
```c
ts_string TSGetHeaderByKey( ts_http* Http, char* Key );   // C version.
ts_string GetHeaderByKey( char* Key );                    // C++ version.
```
Returns the request header value for a given header. To be used when wanting to probe the value of a particular header (ex: the "Host" header).

---
#### GetHeaderByIdx
```c
ts_string TSGetHeaderByIdx( ts_http* Http, size_t Idx );   // C version.
ts_string GetHeaderByIdx( size_t Idx );                    // C++ version.
```
Returns the request header value for the index of the list of headers, starting from `0` until `Http->HeaderCount - 1`. To be used when probing for header values in sequence, usually inside a loop.

---
#### ParseFormData
```c
ts_form TSParseFormData( ts_http* Http );   // C version.
ts_form ParseFormData( void );              // C version.
```
If the request body is of type `Http->EntityType == MimeType_Multipart`, this function can be used to parse it and return a `ts_form` object, that can be then used to probe whichever fields came on the multipart request. Running this parser modifies the request body buffer, making reading directly from it impractical. The `ts_form` struct has the following definition:

```c
typedef struct
{
    size_t FieldCount;   // Number of fields.
    void* FirstField;    // Pointer to first field.
    void* LastField;     // Pointer to last field.
} ts_form;
```
Here, the only value of interest is the `FieldCount` variable, which holds how many fields are there. The `FirstField` and `LastField` variables are for the internal workings of the system only, and aren't meant for consuming directly.

---
#### GetFormFieldByName
```c
ts_form_field TSGetFormFieldByName( ts_form* Form, char* FieldName );   // C version.
ts_form_field GetFormFieldByName( ts_form* Form, char* FieldName );     // C++ version.
```
Gets the multipart field fully parsed and ready to consume, given a specific field name. The field is returned as a `ts_form_field` object, which has the following definition:

```c
typedef struct
{
    ts_string FieldName;   // Name of the field.
    ts_string Filename;    // Name of the file, if the field is a file (blank otherwise).
    ts_string Charset;     // The charset of the data, if present (blank otherwise).
    void* Data;            // Pointer to the raw data.
    uint32_t DataLen;      // Size of the raw data, in bytes.
} ts_form_field;
```
The variables `FieldName`, `Filename`, and `Charset` are the metadata of the field, whereas the `Data` and `DataLen` are for accessing the field value. Do note that DataLen is the size of the data in bytes, so in case of a text field it may not accurately represent the number of characters if the message comes in a multi-byte encoding (such as UTF-8).

---
#### GetFormFieldByIdx
```c
ts_form_field TSGetFormFieldByIdx( ts_form* Form, char* FieldIdx );   // C version.
ts_form_field GetFormFieldByIdx( ts_form* Form, char* FieldIdx );     // C++ version.
```
Same as the method above, but gets the multipart field given an index value, from `0` to `[ts_form].FieldCount - 1`. To be used when probing for fields in sequence, usually inside a loop.

---
#### SetReturnCode
```c
bool TSSetReturnCode( ts_http* Http, size_t ReturnCode );   // C version.
bool SetReturnCode( size_t ReturnCode );                    // C++ version.
```
Sets the status code for the HTTP response header, according to the [IETF specifications](https://datatracker.ietf.org/doc/html/rfc7231#section-6). If a status code not found in that list is returned, the server overwrites it and returns code `500 - Internal Server Error` instead, and the function returns `false`. Otherwise the function returns `true`.

---
#### AllocContentBuffer
```c
ts_string TSAllocContentBuffer( ts_http* Http, size_t  BufferSize );   // C version.
ts_string AllocContentBuffer( size_t  BufferSize );                    // C++ version.
```
Grabs a chunk of memory from the system, zeroed out, to be used for writing the response content into. The application should not attempt to free or realloc it, nor to write past its requested size (buffers are allocated at page size granularity, so it may be that a bit more of the requested size will be allocated, but the page immediately after it is protected and attempts to write to it will trigger an exception, that must be handled by the application). The actual allocated size is returned in the `ts_string.MaxSize` parameter.

If this method is called, the `SetContentSize` method must be called as well, and preferably also the `SetContentType` method detailed below. If this function is not called, no response body is sent out.

---
#### SetContentSize
```c
void TSSetContentSize( ts_http* Http, size_t ContentSize );   // C version.
void SetContentSize( size_t ContentSize );                    // C++ version.
```
Sets the size of the response body to be sent out. This may be the same as the `ts_string.MaxSize` value of the allocated content buffer, or smaller, but never higher. If this method is not called, no response body is sent out.

---
#### SetContentType
```c
bool TSSetContentType( ts_http* Http, char* ContentType );   // C version.
bool SetContentType( char* ContentType );                    // C++ version.
```
Sets the content type of the response body to be sent out. This type must be one of the media types present in the [IANA Media Types](https://www.iana.org/assignments/media-types/media-types.xhtml) page, together with the mime type register (ex: if the content is a JPG, this content type must read `image/jpeg`). The content type must be passed as a pointer to a buffer containing the type as text, null-terminated (this data is copied to an internal system buffer, so the buffer that holds the text can be freed as soon as the function returns).

If this method is not called (or if it's called with an incorrect content type), the returned body is sent as type `application/octet-stream`.

This function returns `true` if it succeeds, and `false` otherwise.

---
#### AllocCookiesBuffer
```c
ts_string TSAllocCookiesBuffer( ts_http* Http, size_t BufferSize );   // C version.
ts_string AllocCookiesBuffer( size_t BufferSize );                    // C++ version.
```
Same as the method above, but it allocates memory space to be used for the returned cookies. This buffer is for setting the entire cookie return structure, including the `Set-Cookie:` header field and the appended qualifiers, all which are left on the responsibility of the application. This buffer is sent as-is to the client, as part of the response header. For a more practical approach to setting the cookie, see the `RecordCookie` method below.

If this method is called, the `SetContentSize` method must be called as well. In case of several cookies, all of them must be inserted into this buffer, separating each line with a line break (preferably CRLF).

---
#### SetCookiesSize
```c
void TSSetCookieSize( ts_http* Http, size_t CookiesSize );   // C version.
void SetCookieSize( size_t CookiesSize );                    // C++ version.
```
Sets the size of the cookies to be sent out. This may be the same as the `ts_string.MaxSize` value of the allocated cookies buffer, or smaller, but never higher. If this method is not called, no cookies sent out. This method must only be used when manually formatting the cookies with the `AllocCookiesBuffer` function; it should not be used when working with the `RecordCookie` function.

---
#### RecordCookie
```c
bool TSRecordCookie( ts_http* Http, ts_cookie* Cookie );   // C version.
bool RecordCookie( ts_cookie* Cookie );                    // C++ version.
```
This function facilitates setting the cookies, so you don't have to do it manually. It works by passing a pointer to a `ts_cookie` object (defined below), which is then used to create the correct cookie formatting. This function does not require a previous call to `AllocCookiesBuffer`, though it also works after calling it (even after setting one or more cookies manually, so you can interleave manually-formatted cookies and ones done through this function). If the cookies buffer has not been previously allocated, the system allocates one of the maximum size specified in the server conf file.

```c
typedef struct
{
    ts_string Name;           // Name of the cookie.
    ts_string Value;          // Value of the cookie.
    ts_time_format ExpDate;   // Value of the Expires qualifier.
    uint32_t MaxAge;          // Value of the Max-Age qualifier.
    char* Domain;             // Value of the Domain qualifier.
    char* Path;               // Value of the Path qualifier.
    uint32_t AttrFlags;       // Bit-field of qualifiers to be set.
} ts_cookie;
```
The Name and Value fields are for setting the `<name=value>` part of the cookie, and is a required information. The other fields are for the qualifiers, which are optional. ExpDate sets the `Expires` qualifier, and is set with the `ts_time_format` struct defined below; the MaxAge field sets the `Max-Age` qualifier, whereas Domain and Path set `Domain` and `Path` qualifiers respectively (must be a pointer to a null-terminated string). The `SameSite`, `HttpOnly`, and `Secure` qualifiers are set as bit-flags in the AttrFlags field.

Any qualifier used must be set in the AttrFlags field, which is a bit-field that should receive one or more of the bit flags present in the `ts_cookie_attr` enum, defined in the header file. Any qualifier not set to this field will be ignored, even if its value was set on the previous fields. As an example:

```c++
ts_cookie Cookie;
Cookie.Name = /* ts_string with name */ ;
Cookie.Value = /* ts_string with value */ ;
Cookie.MaxAge = 3600;
Cookie.Path = "/internal";
Cookie.AttrFlags = CookieAttr_MaxAge | CookieAttr_SameSiteStrict;
```
The example above sets the Path field, but forgets to set the `CookieAttr_Path` flag and, as such, the qualifier is ignored.

For the ExpDate field, the date is set with the struct below:
```c
typedef struct
{
    uint32_t Year;
    uint32_t Month;
    uint32_t Day;
    uint32_t Hour;
    uint32_t Minute;
    uint32_t Second;
    ts_week_day WeekDay;
} ts_time_format;
```
where each field sets a part of the date. The WeekDay field is of the `ts_week_day` enum, defined in the header file. All values must be set so that the correct IMF datetime format can be constructed. Any inconsistency in it may be interpreted by the client erroneously, and result in a cookie time limit different than the one desired.

This function returns `true` if the cookie was correctly set in the buffer, and `false` otherwise (usually due to buffer size limit). In case of failure, no data is recorded.


