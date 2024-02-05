#include "tinybase-strings.h"

//================================
// Helper functions
//================================

#define JumpCLRF(Ptr, Cur, Size) do { \
Cur += (Size - Cur >= 1 && Ptr[Cur] == '\r') ? 1 : 0; \
Cur += (Size - Cur >= 1 && Ptr[Cur] == '\n') ? 1 : 0; \
} while (0);
#define JumpBackCLRF(Ptr, Cur) do { \
Cur -= (Cur >= 1 && Ptr[Cur-1] == '\n') ? 1 : 0; \
Cur -= (Cur >= 1 && Ptr[Cur-1] == '\r') ? 1 : 0; \
} while (0);
#define StripCLRF(Buffer) do { \
Buffer.WriteCur -= (Buffer.WriteCur >= 1 && Buffer.Base[Buffer.WriteCur-1] == '\n') ? 1 : 0; \
Buffer.WriteCur -= (Buffer.WriteCur >= 1 && Buffer.Base[Buffer.WriteCur-1] == '\r') ? 1 : 0; \
} while (0);

internal string
EatToken(string Src, usz* ReadCur, char Token)
{
    string Result = { 0, 0, 0, EC_ASCII };
    
    string Search = String(Src.Base + *ReadCur, Src.WriteCur - *ReadCur, 0, EC_ASCII);
    usz TokenCur = CharInString(Token, Search, RETURN_IDX_FIND);
    if (TokenCur != INVALID_IDX)
    {
        Result.Base = Search.Base;
        Result.WriteCur = TokenCur;
        *ReadCur += TokenCur + 1;
    }
    
    return Result;
}

internal string
EatSubstring(string Src, usz* ReadCur, char StartToken, char EndToken)
{
    string Result = { 0, 0, 0, EC_ASCII };
    
    string Search = String(Src.Base + *ReadCur, Src.WriteCur - *ReadCur, 0, EC_ASCII);
    char* Substring = (char*)CharInString(StartToken, Search, RETURN_PTR_AFTER);
    if (Substring)
    {
        usz Diff = Substring - Search.Base;
        usz TmpReadCur = *ReadCur + Diff;
        AdvanceBuffer(&Search.Buffer, Diff);
        
        usz SubstringSize = CharInString(EndToken, Search, RETURN_IDX_FIND);
        if (SubstringSize != INVALID_IDX)
        {
            Result.Base = Substring;
            Result.WriteCur = SubstringSize;
            *ReadCur = TmpReadCur + SubstringSize + 1; // Eat the [EndToken].
        }
    }
    
    return Result;
}

internal void
PercentEncodingToUTF8(string Src, string* Dst)
{
    usz FoundIdx = 0;
    while ((FoundIdx = CharInString('%', Src, RETURN_IDX_FIND)) != INVALID_IDX)
    {
        CopyData(Dst->Base + Dst->WriteCur, Dst->Size, Src.Base, FoundIdx);
        Dst->WriteCur += FoundIdx;
        string Byte = String(Src.Base + FoundIdx + 1, 2, 0, Src.Enc);
        Dst->Base[Dst->WriteCur++] = (u8)StringToHex(Byte);
        
        Src.Base += FoundIdx + 3;
        Src.WriteCur -= FoundIdx + 3;
    }
    CopyData(Dst->Base + Dst->WriteCur, Dst->Size, Src.Base, Src.WriteCur);
    Dst->WriteCur += Src.WriteCur;
    
    ReplaceByteInBuffer('+', ' ', Dst->Buffer);
}

//================================
// Parsing request header
//================================

internal bool
IsRequestMalicious(string Path, string Query)
{
    // Protects against path-traversal.
    isz DirIdx = 0;
    usz PathCur = Path.Base[0] == '/' ? 1 : 0;
    while (DirIdx >= 0)
    {
        string Dir = EatToken(Path, &PathCur, '/');
        if (Dir.Base == NULL) break;
        
        if (Dir.WriteCur == 1 && Dir.Base[0] == '.') continue;
        else if (Dir.WriteCur == 2 && Dir.Base[0] == '.' && Dir.Base[1] == '.') DirIdx--;
        else DirIdx++;
    }
    
    if (DirIdx >= 0)
    {
        // Protects against XSS.
        if (Query.WriteCur == 0) return false;
        if (!CharInString('<', Query, RETURN_BOOL)
            && !CharInString('>', Query, RETURN_BOOL)
            && !CharInString('\"', Query, RETURN_BOOL)) return false;
    }
    
    return true;
}

external ts_http_parse
ParseHttpHeader(string InBuffer, ts_request* Request)
{
    usz ReadCur = 0;
    
    // Checks if first line has been parsed.
    if (!Request->FirstHeaderOffset)
    {
        Request->Base = (char*)InBuffer.Base;
        
        string Line = EatToken(InBuffer, &ReadCur, '\n');
        if (Line.WriteCur == 0)
        {
            return HttpParse_HeaderIncomplete;
        }
        usz LineReadCur = 0;
        
        // Parse Verb
        string Verb = EatToken(Line, &LineReadCur, ' ');
        if (Verb.WriteCur == 0)
        {
            return HttpParse_HeaderInvalid;
        }
        
        if (EqualStrings(Verb, StringLit("GET"))) Request->Verb = HttpVerb_Get;
        else if (EqualStrings(Verb, StringLit("HEAD"))) Request->Verb = HttpVerb_Head;
        else if (EqualStrings(Verb, StringLit("POST"))) Request->Verb = HttpVerb_Post;
        else if (EqualStrings(Verb, StringLit("PUT"))) Request->Verb = HttpVerb_Put;
        else if (EqualStrings(Verb, StringLit("DELETE"))) Request->Verb = HttpVerb_Delete;
        else if (EqualStrings(Verb, StringLit("CONNECT"))) Request->Verb = HttpVerb_Connect;
        else if (EqualStrings(Verb, StringLit("OPTIONS"))) Request->Verb = HttpVerb_Options;
        else if (EqualStrings(Verb, StringLit("TRACE"))) Request->Verb = HttpVerb_Trace;
        else if (EqualStrings(Verb, StringLit("PATCH"))) Request->Verb = HttpVerb_Patch;
        else
        {
            return HttpParse_HeaderInvalid;
        }
        
        // Parse URI
        string Uri = EatToken(Line, &LineReadCur, ' ');
        if (Uri.WriteCur == 0)
        {
            // No version after URI means HTTP 0.9
            Uri.Base = Line.Base + Verb.WriteCur + 1;
            Uri.WriteCur = Line.WriteCur - Verb.WriteCur - 1;
            JumpBackCLRF(Uri.Base, Uri.WriteCur);
            LineReadCur = Line.WriteCur;
        }
        Request->UriOffset = Verb.WriteCur + 1;
        
        string UriDecoded = String(Uri.Base, 0, Uri.WriteCur, EC_UTF8);
        PercentEncodingToUTF8(Uri, &UriDecoded);
        
        usz QueryToken = CharInString('?', UriDecoded, RETURN_IDX_FIND);
        usz PathSize = (QueryToken == INVALID_IDX) ? UriDecoded.WriteCur : QueryToken;
        usz QuerySize = (QueryToken == INVALID_IDX) ? 0 : UriDecoded.WriteCur - (QueryToken+1);
        
        string UriPath = String(Uri.Base, PathSize, 0, EC_UTF8);
        string UriQuery = String(Uri.Base + PathSize + 1, QuerySize, 0, EC_UTF8);
        
        if (IsRequestMalicious(UriPath, UriQuery))
        {
            return HttpParse_HeaderMalicious;
        }
        
        Request->PathSize = PathSize;
        Request->QuerySize = QuerySize;
        
        // Parse Version
        string Version = String(Line.Base + LineReadCur, Line.WriteCur - LineReadCur,
                                0, EC_ASCII);
        StripCLRF(Version);
        
        if (Version.WriteCur == 0)
            Request->Version = HttpVersion_09;
        else if (EqualStrings(Version, StringLit("HTTP/1.0")))
            Request->Version = HttpVersion_10;
        else if (EqualStrings(Version, StringLit("HTTP/1.1")))
            Request->Version = HttpVersion_11;
        else if (EqualStrings(Version, StringLit("HTTP/2.0")))
            Request->Version = HttpVersion_20;
        else
        {
            return HttpParse_HeaderInvalid;
        }
        
        Request->HeaderSize = ReadCur;
        Request->FirstHeaderOffset = ReadCur - 1;
    }
    
    // Parse Headers
    ReadCur = Request->HeaderSize;
    string Line = EatToken(InBuffer, &ReadCur, '\n');
    while (ReadCur != INVALID_IDX)
    {
        Request->HeaderSize = ReadCur;
        
        if (Line.Base[0] == '\r'
            || Line.Base[0] == '\n')
        {
            return HttpParse_OK;
        }
        
        if (Request->NumHeaders == MAX_NUM_HEADERS)
        {
            return HttpParse_TooManyHeaders;
        }
        
        usz LineReadCur = 0;
        
        u8* KeySizeInfo = (u8*)(Line.Base - 1);
        string Key = EatToken(Line, &LineReadCur, ':');
        if (Key.WriteCur == 0 || Key.WriteCur >= 0xFF)
        {
            return HttpParse_HeaderInvalid;
        }
        LineReadCur++;
        
        u16* ValueSizeInfo = (u16*)(Line.Base + LineReadCur - 2);
        string Value = String(Line.Base + LineReadCur, Line.WriteCur - LineReadCur,
                              0, EC_ASCII);
        if (Value.WriteCur == 0 || Key.WriteCur >= 0xFFFF)
        {
            return HttpParse_HeaderInvalid;
        }
        
        *KeySizeInfo = (u8)Key.WriteCur;
        *ValueSizeInfo = (u16)Value.WriteCur;
        
        Request->NumHeaders++;
        
        Line = EatToken(InBuffer, &ReadCur, '\n');
    }
    
    return HttpParse_HeaderIncomplete;
}

internal bool
HeadersAreEqual(string Header, char* TargetStr)
{
    string Target = String(TargetStr, strlen(TargetStr), 0, EC_ASCII);
    return EqualStrings(Header, Target);
}

external string
GetHeaderByKey(ts_request* Request, char* TargetKey)
{
    string Value = { 0, 0, 0, EC_ASCII };
    
    char* CurrentKeyPtr = Request->Base + Request->FirstHeaderOffset;
    for (usz Count = 0; Count < Request->NumHeaders; Count++)
    {
        u8 KeySize = *CurrentKeyPtr;
        string CurrentKey = String(CurrentKeyPtr + sizeof(u8), KeySize, 0, EC_ASCII);
        char* CurrentValuePtr = CurrentKeyPtr + sizeof(u8) + KeySize;
        u16 ValueSize = *(u16*)CurrentValuePtr;
        
        if (HeadersAreEqual(CurrentKey, TargetKey))
        {
            Value.Base = CurrentValuePtr + sizeof(u16);
            Value.WriteCur = ValueSize;
            StripCLRF(Value);
            break;
        }
        
        CurrentKeyPtr = CurrentValuePtr + sizeof(u16) + ValueSize;
    }
    
    return Value;
}

external string
GetHeaderByIdx(ts_request* Request, usz TargetIdx)
{
    string Value = { 0, 0, 0, EC_ASCII };
    
    if (TargetIdx < Request->NumHeaders)
    {
        char* CurrentKeyPtr = Request->Base + Request->FirstHeaderOffset;
        for (usz Count = 0; Count < TargetIdx; Count++)
        {
            u8 KeySize = *CurrentKeyPtr;
            u16 ValueSize = *(u16*)(CurrentKeyPtr + sizeof(u8) + KeySize);
            CurrentKeyPtr += KeySize + sizeof(u16) + ValueSize;
        }
        u8 KeySize = *(u8*)CurrentKeyPtr;
        char* CurrentValuePtr = CurrentKeyPtr + sizeof(u8) + KeySize;
        
        Value.Base = CurrentValuePtr + sizeof(u16);
        Value.WriteCur = *(u16*)CurrentValuePtr;
        StripCLRF(Value);
    }
    
    return Value;
}

//================================
// Parsing request body
//================================

#pragma pack(push, 1)
typedef struct ts_form_field_parser
{
    u8 IsFile;
    u8 FieldNameOffset;
    u16 FieldNameLen;
    u8 FilenameOffset;
    u16 FilenameLen;
    u8 CharsetOffset;
    u16 CharsetLen;
    u8 DataOffset;
    u32 DataLen;
    u16 NextFieldOffset;
} ts_form_field_parser;
#pragma pack(pop)

typedef enum ts_form_parsing_stage
{
    FormParse_FirstLine,
    FormParse_SecondLine,
    FormParse_ThirdLine,
    FormParse_Data,
    FormParse_Complete,
    FormParse_Error
} ts_form_parsing_stage;

external ts_body
GetBodyInfo(ts_request* Request)
{
    ts_body Result = {0};
    
    string EntitySize = GetHeaderByKey(Request, "Content-Length");
    string ContentType = GetHeaderByKey(Request, "Content-Type");
    
    usz BodySize;
    if (EntitySize.Base
        && ContentType.Base
        && (BodySize = StringToUInt(EntitySize)) != USZ_MAX)
    {
        Result.Base = (u8*)Request->Base + Request->HeaderSize;
        Result.Size = BodySize;
        Result.ContentType = ContentType.Base;
        Result.ContentTypeSize = ContentType.WriteCur;
    }
    
    return Result;
}

external ts_multiform
ParseFormData(ts_body RequestBody)
{
    ts_multiform Form = {0}, EmptyForm = {0};
    
    // Field boundary is evaluated with a -- before it, so we include it before the
    // string in the header to facilitate search.
    
    string EntityType = String(RequestBody.ContentType, RequestBody.ContentTypeSize,
                               0, EC_ASCII);
    char* BoundaryPtr = (char*)CharInString('=', EntityType, RETURN_PTR_FIND);
    *BoundaryPtr-- = '-';
    *BoundaryPtr   = '-';
    string Boundary = String(BoundaryPtr, EntityType.Base+EntityType.WriteCur-BoundaryPtr,
                             0, EC_ASCII);
    
    // Final boundary ends in "--" (e.g. if boundary is "--s20bHc", final boundary
    // will be "--s20bHc--").
    
    usz FinalBoundarySize = Boundary.Size + 2;
    
    string Body = String(RequestBody.Base, RequestBody.Size, 0, EC_ASCII);
    usz ReadCur = 0;
    char* Offset = NULL;
    ts_form_field_parser Parser = {0};
    ts_form_field_parser* CurrentField = NULL;
    
    // Jumps the first boundary, because it's useless for us.
    string Line = EatToken(Body, &ReadCur, '\n');
    ts_form_parsing_stage ParseStage = FormParse_FirstLine;
    
    bool FormIsValid = true;
    
    while (FormIsValid && ReadCur < Body.WriteCur)
    {
        if (ParseStage == FormParse_FirstLine)
        {
            Line = EatToken(Body, &ReadCur, '\n');
            if (Line.Base)
            {
                string Content = StringLit("Content-Disposition: form-data");
                if (CompareStrings(Line, Content, Content.WriteCur, RETURN_BOOL))
                {
                    usz LineCur = Content.WriteCur;
                    CurrentField = (ts_form_field_parser*)Line.Base;
                    
                    // name="Field".
                    string FieldName = EatSubstring(Line, &LineCur, '\"', '\"');
                    usz FieldNameOffset = FieldName.Base - Line.Base;
                    if (FieldNameOffset <= U8_MAX
                        && FieldName.WriteCur <= U16_MAX)
                    {
                        Parser.FieldNameOffset = (u8)FieldNameOffset;
                        Parser.FieldNameLen = (u16)FieldName.WriteCur;
                        Offset = Line.Base + FieldNameOffset + FieldName.WriteCur;
                        
                        // If field is file, format is:
                        //   [name="FieldName";filename="File.ext"].
                        if (Line.Base[LineCur] == ';')
                        {
                            Parser.IsFile = true;
                            LineCur++;
                            
                            // filename="File.ext".
                            string Filename = EatSubstring(Line, &LineCur, '\"', '\"');
                            usz FilenameOffset = Filename.Base - Offset;
                            if (FilenameOffset <= U8_MAX
                                && Filename.WriteCur <= U16_MAX)
                            {
                                Parser.FilenameOffset = (u8)FilenameOffset;
                                Parser.FilenameLen = (u16)Filename.WriteCur;
                                Offset += FilenameOffset + Filename.WriteCur;
                            }
                            else
                            {
                                FormIsValid = false;
                            }
                        }
                        
                        ParseStage = FormParse_SecondLine;
                    }
                    else
                    {
                        FormIsValid = false;
                    }
                }
                else
                {
                    FormIsValid = false;
                }
            }
            else
            {
                FormIsValid = false;
            }
        }
        
        else if (ParseStage == FormParse_SecondLine)
        {
            Line = EatToken(Body, &ReadCur, '\n');
            if (Line.Base)
            {
                usz LineCur = 0;
                
                // If line is not blank, it can only be Content-Type.
                string Content = StringLit("Content-Type");
                if (CompareStrings(Line, Content, Content.WriteCur, RETURN_BOOL))
                {
                    usz LineCur = Content.WriteCur;
                    
                    // Ignores Content-Type (it can later be retrieved from file ext).
                    // Checks to see if file has encoding information, format is:
                    //   [charset=Encoding].
                    string Encoding = EatSubstring(Line, &LineCur, '=', '\n');
                    if (Encoding.Base)
                    {
                        usz CharsetOffset = Encoding.Base - Offset;
                        if (CharsetOffset <= U8_MAX
                            && Encoding.WriteCur <= U16_MAX)
                        {
                            Parser.CharsetOffset = (u8)CharsetOffset;
                            Parser.CharsetLen = (u16)Encoding.WriteCur;
                            Offset += CharsetOffset + Encoding.WriteCur;
                        }
                    }
                    
                    ParseStage = FormParse_ThirdLine;
                }
                
                // Blank line points to beginning of data.
                else if (Line.Base[LineCur] == '\r'
                         || Line.Base[LineCur] == '\n')
                {
                    Parser.DataOffset = (u8)(Body.Base + ReadCur - Offset);
                    
                    *CurrentField = Parser;
                    Form.LastField = CurrentField;
                    if (!Form.FirstField)
                    {
                        Form.FirstField = CurrentField;
                    }
                    ParseStage = FormParse_Data;
                }
                else
                {
                    FormIsValid = false;
                }
            }
            else
            {
                FormIsValid = false;
            }
        }
        
        else if (ParseStage == FormParse_ThirdLine)
        {
            Line = EatToken(Body, &ReadCur, '\n');
            if (Line.Base)
            {
                // Third line can only be blank.
                if (Line.Base[0] == '\r'
                    || Line.Base[0] == '\n')
                {
                    Parser.DataOffset = (u8)(Body.Base + ReadCur - Offset);
                    
                    *CurrentField = Parser;
                    Form.LastField = CurrentField;
                    if (!Form.FirstField)
                    {
                        Form.FirstField = CurrentField;
                    }
                    ParseStage = FormParse_Data;
                }
                else
                {
                    FormIsValid = false;
                }
            }
            else
            {
                FormIsValid = false;
            }
        }
        
        else if (ParseStage == FormParse_Data)
        {
            buffer RawData = Buffer(Body.Base + ReadCur, Body.WriteCur - ReadCur, 0);
            usz DataEnd = BufferInBuffer(Boundary.Buffer, RawData, RETURN_IDX_FIND);
            if (DataEnd != INVALID_IDX)
            {
                ReadCur += DataEnd + Boundary.WriteCur;
                
                JumpBackCLRF(RawData.Base, DataEnd);
                Parser.DataLen = (u32)DataEnd;
                
                Form.FieldCount++;
                if (Body.Base[ReadCur] == '-')
                {
                    ParseStage = FormParse_Complete;
                }
                else if (Body.Base[ReadCur] == '\r'
                         || Body.Base[ReadCur] == '\n')
                {
                    JumpCLRF(Body.Base, ReadCur, Body.WriteCur);
                    Parser.NextFieldOffset = (u16)((Body.Base + ReadCur)
                                                   - ((char*)RawData.Base + DataEnd));
                    ParseStage = FormParse_FirstLine;
                }
                else
                {
                    FormIsValid = false;
                }
                
                CurrentField->DataLen = Parser.DataLen;
                CurrentField->NextFieldOffset = Parser.NextFieldOffset;
                
                memset(&Parser, 0, sizeof(Parser));
                CurrentField = (ts_form_field_parser*)(Body.Base + ReadCur);
            }
            else
            {
                FormIsValid = false;
            }
        }
        
        else if (ParseStage == FormParse_Complete)
        {
            break;
        }
        
        else
        {
            FormIsValid = false;
        }
    }
    
    return FormIsValid ? Form : EmptyForm;
}

internal ts_form_field
OrganizeFieldInfo(ts_form_field_parser* Parser)
{
    ts_form_field Result = {0};
    
    char* Ptr = (char*)Parser;
    Result.FieldName = Ptr + Parser->FieldNameOffset;
    Result.FieldNameSize = Parser->FieldNameLen;
    Ptr += Parser->FieldNameOffset + Parser->FieldNameLen;
    
    if (Parser->FilenameLen)
    {
        Result.Filename = Ptr + Parser->FilenameOffset;
        Result.FilenameSize = Parser->FilenameLen;
        Ptr += Parser->FilenameOffset + Parser->FilenameLen;
    }
    if (Parser->CharsetLen)
    {
        Result.Charset = Ptr + Parser->CharsetOffset;
        Result.CharsetSize = Parser->CharsetLen;
        Ptr += Parser->CharsetOffset + Parser->CharsetLen;
    }
    
    Result.Data = Ptr + Parser->DataOffset;
    Result.DataLen = Parser->DataLen;
    
    return Result;
}

external ts_form_field
GetFormFieldByName(ts_multiform Form, char* TargetName)
{
    ts_form_field Result = {0};
    
    string Target = String(TargetName, strlen(TargetName), 0, EC_ASCII);
    ts_form_field_parser* TargetField = (ts_form_field_parser*)Form.FirstField;
    for (usz Count = 0; Count < Form.FieldCount; Count++)
    {
        char* Ptr = (char*)TargetField;
        string FieldName = String(Ptr + TargetField->FieldNameOffset,
                                  TargetField->FieldNameLen, 0, EC_ASCII);
        
        if (EqualStrings(FieldName, Target))
        {
            Result = OrganizeFieldInfo(TargetField);
            break;
        }
        
        usz NextFieldOffset = (TargetField->FieldNameOffset + TargetField->FieldNameLen +
                               TargetField->FilenameOffset + TargetField->FilenameLen +
                               TargetField->CharsetOffset + TargetField->CharsetLen +
                               TargetField->DataOffset + TargetField->DataLen +
                               TargetField->NextFieldOffset);
        TargetField = (ts_form_field_parser*)(Ptr + NextFieldOffset);
    }
    
    return Result;
}

external ts_form_field
GetFormFieldByIdx(ts_multiform Form, usz TargetIdx)
{
    ts_form_field Result = {0};
    
    if (TargetIdx < Form.FieldCount)
    {
        ts_form_field_parser* TargetField = (ts_form_field_parser*)Form.FirstField;
        for (usz Count = 0; Count < TargetIdx; Count++)
        {
            usz NextFieldOffset = (TargetField->FieldNameOffset + TargetField->FieldNameLen +
                                   TargetField->FilenameOffset + TargetField->FilenameLen +
                                   TargetField->CharsetOffset + TargetField->CharsetLen +
                                   TargetField->DataOffset + TargetField->DataLen +
                                   TargetField->NextFieldOffset);
            TargetField = (ts_form_field_parser*)((u8*)TargetField + NextFieldOffset);
        }
        Result = OrganizeFieldInfo(TargetField);
    }
    
    return Result;
}

//================================
// Response
//================================

internal bool
FormatTimeIMF(datetime TimeFormat, string* Dst)
{
    bool Result = false;
    
    if (Dst->Size >= (Dst->WriteCur + IMF_DATE_LENGTH))
    {
        string WeekDay = { 0, 3, 0, EC_ASCII};
        switch (TimeFormat.WeekDay)
        {
            case WeekDay_Sunday: WeekDay.Base = "Sun"; break;
            case WeekDay_Monday: WeekDay.Base = "Mon"; break;
            case WeekDay_Tuesday: WeekDay.Base = "Tue"; break;
            case WeekDay_Wednesday: WeekDay.Base = "Wed"; break;
            case WeekDay_Thursday: WeekDay.Base = "Thu"; break;
            case WeekDay_Friday: WeekDay.Base = "Fri"; break;
            case WeekDay_Saturday: WeekDay.Base = "Sat"; break;
        }
        AppendStringToString(WeekDay, Dst);
        
        AppendStringToString(StringLit(", "), Dst);
        if (TimeFormat.Day < 10) AppendStringToString(StringLit("0"), Dst);
        AppendIntToString(TimeFormat.Day, Dst);
        AppendStringToString(StringLit(" "), Dst);
        
        string Month = { 0, 3, 0, EC_ASCII };
        switch (TimeFormat.Month)
        {
            case 1: Month.Base = "Jan"; break;
            case 2: Month.Base = "Feb"; break;
            case 3: Month.Base = "Mar"; break;
            case 4: Month.Base = "Apr"; break;
            case 5: Month.Base = "May"; break;
            case 6: Month.Base = "Jun"; break;
            case 7: Month.Base = "Jul"; break;
            case 8: Month.Base = "Aug"; break;
            case 9: Month.Base = "Sep"; break;
            case 10: Month.Base = "Oct"; break;
            case 11: Month.Base = "Nov"; break;
            case 12: Month.Base = "Dec"; break;
        }
        AppendStringToString(Month, Dst);
        
        AppendStringToString(StringLit(" "), Dst);
        AppendIntToString(TimeFormat.Year, Dst);
        AppendStringToString(StringLit(" "), Dst);
        if (TimeFormat.Hour < 10) AppendStringToString(StringLit("0"), Dst);
        AppendIntToString(TimeFormat.Hour, Dst);
        AppendStringToString(StringLit(" "), Dst);
        if (TimeFormat.Minute < 10) AppendStringToString(StringLit("0"), Dst);
        AppendIntToString(TimeFormat.Minute, Dst);
        AppendStringToString(StringLit(" "), Dst);
        if (TimeFormat.Second < 10) AppendStringToString(StringLit("0"), Dst);
        AppendIntToString(TimeFormat.Second, Dst);
        AppendStringToString(StringLit(" GMT"), Dst);
        
        Result = true;
    }
    
    return Result;
}

external void
CraftHttpResponseHeader(ts_response* Response, string* Header, _opt char* ServerName)
{
    string LineBreak = StringLit("\r\n");
    if (!ServerName)
    {
        ServerName = "TinyServer";
    }
    
    //===================
    // Mandatory fields.
    //===================
    
    string Version = {0};
    switch (Response->Version)
    {
        case HttpVersion_09: Version = StringLit("HTTP/0.9 "); break;
        case HttpVersion_10: Version = StringLit("HTTP/1.0 "); break;
        default:
        case HttpVersion_11: Version = StringLit("HTTP/1.1 "); break;
        case HttpVersion_20: Version = StringLit("HTTP/2.0 "); break;
    }
    AppendStringToString(Version, Header);
    
    string StatusCode = {0};
    switch (Response->StatusCode)
    {
        case 100: StatusCode = StringLit("100 Continue"); break;
        case 101: StatusCode = StringLit("101 Switching Protocol"); break;
        case 200: StatusCode = StringLit("200 OK"); break;
        case 201: StatusCode = StringLit("201 Created"); break;
        case 202: StatusCode = StringLit("202 Accepted"); break;
        case 203: StatusCode = StringLit("203 Non-Authoritative Information"); break;
        case 204: StatusCode = StringLit("204 No Content"); break;
        case 205: StatusCode = StringLit("205 Reset Content"); break;
        case 300: StatusCode = StringLit("300 Multiple Choices"); break;
        case 301: StatusCode = StringLit("301 Moved Permanently"); break;
        case 302: StatusCode = StringLit("302 Found"); break;
        case 303: StatusCode = StringLit("303 See Other"); break;
        case 304: StatusCode = StringLit("304 No Modified"); break;
        case 305: StatusCode = StringLit("305 Use Proxy"); break;
        case 307: StatusCode = StringLit("307 Temporary Redirect"); break;
        case 308: StatusCode = StringLit("308 Permanent Redirect"); break;
        case 400: StatusCode = StringLit("400 Bad Request"); break;
        case 401: StatusCode = StringLit("401 Unauthorized"); break;
        case 402: StatusCode = StringLit("402 Payment Required"); break;
        case 403: StatusCode = StringLit("403 Forbidden"); break;
        case 404: StatusCode = StringLit("404 Not Found"); break;
        case 405: StatusCode = StringLit("405 Method Not Allowed"); break;
        case 406: StatusCode = StringLit("406 Not Acceptable"); break;
        case 407: StatusCode = StringLit("407 Proxy Authorization Required"); break;
        case 408: StatusCode = StringLit("408 Request Timeout"); break;
        case 409: StatusCode = StringLit("409 Conflict"); break;
        case 410: StatusCode = StringLit("410 Gone"); break;
        case 411: StatusCode = StringLit("411 Length Required"); break;
        case 412: StatusCode = StringLit("412 Precondition Failed"); break;
        case 413: StatusCode = StringLit("413 Payload Too Large"); break;
        case 414: StatusCode = StringLit("414 URI Too Long"); break;
        case 415: StatusCode = StringLit("415 Unsupported Media Type"); break;
        case 416: StatusCode = StringLit("416 Range Not Satisfiable"); break;
        case 417: StatusCode = StringLit("417 Expectation Failed"); break;
        case 418: StatusCode = StringLit("418 I'm a Teapot"); break;
        case 421: StatusCode = StringLit("421 Misdirected Request"); break;
        case 425: StatusCode = StringLit("425 Too Early"); break;
        case 426: StatusCode = StringLit("426 Update Required"); break;
        case 428: StatusCode = StringLit("428 Precondition Required"); break;
        case 429: StatusCode = StringLit("429 Too Many Requests"); break;
        case 431: StatusCode = StringLit("431 Request Header Fields Too Large"); break;
        case 440: StatusCode = StringLit("440 Login Timeout"); break;
        case 451: StatusCode = StringLit("451 Unavailable for Legal Reasons"); break;
        case 500: StatusCode = StringLit("500 Internal Server Error"); break;
        case 501: StatusCode = StringLit("501 Not Implemented"); break;
        case 502: StatusCode = StringLit("502 Bad Gateway"); break;
        case 503: StatusCode = StringLit("503 Service Unavailable"); break;
        case 504: StatusCode = StringLit("504 Gateway Timeout"); break;
        case 505: StatusCode = StringLit("505 HTTP Version Not Supported"); break;
        case 507: StatusCode = StringLit("507 Insufficient Storage"); break;
        case 510: StatusCode = StringLit("510 Not Extended"); break;
        case 511: StatusCode = StringLit("511 Network Authentication Required"); break;
        
        default:
        {
            // TODO: Free Payload and Cookies, and return generic 500 error header.
        }
    }
    
    AppendStringToString(StatusCode, Header);
    AppendStringToString(LineBreak, Header);
    
    AppendStringToString(StringLit("Date: "), Header);
    FormatTimeIMF(CurrentSystemTime(), Header);
    AppendStringToString(LineBreak, Header);
    
    AppendStringToString(StringLit("Server: "), Header);
    AppendArrayToString(ServerName, Header);
    AppendStringToString(LineBreak, Header);
    
    AppendStringToString(StringLit("Access-Control-Allow-Origin: *"), Header);
    AppendStringToString(LineBreak, Header);
    
    AppendStringToString(StringLit("Connection: "), Header);
    string Connection = Response->KeepAlive ? StringLit("keep-alive") : StringLit("close");
    AppendStringToString(Connection, Header);
    AppendStringToString(LineBreak, Header);
    
    AppendStringToString(StringLit("Content-Length: "), Header);
    AppendIntToString(Response->PayloadSize, Header);
    AppendStringToString(LineBreak, Header);
    
    //==================
    // Optional fields.
    //==================
    
    if (Response->PayloadSize > 0)
    {
        AppendStringToString(StringLit("Content-Type: "), Header);
        AppendArrayToString(Response->MimeType, Header);
        AppendStringToString(LineBreak, Header);
    }
    
    if (Response->CookiesSize == 0)
    {
        // If there are cookies to be sent, final newline goes in cookies buffer,
        // else it goes in here.
        
        AppendStringToString(LineBreak, Header);
    }
    
    Response->HeaderSize = Header->WriteCur;
}
