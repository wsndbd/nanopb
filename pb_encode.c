/* pb_encode.c -- encode a protobuf using minimal resources
 *
 * 2011 Petteri Aimonen <jpa@kapsi.fi>
 */

#define NANOPB_INTERNALS
#include "pb.h"
#include "pb_encode.h"

/* Use the GCC warn_unused_result attribute to check that all return values
 * are propagated correctly. On other compilers and gcc before 3.4.0 just
 * ignore the annotation.
 */
#if !defined(__GNUC__) || ( __GNUC__ < 3) || (__GNUC__ == 3 && __GNUC_MINOR__ < 4)
    #define checkreturn
#else
    #define checkreturn __attribute__((warn_unused_result))
#endif

/**************************************
 * Declarations internal to this file *
 **************************************/
static bool checkreturn buf_write(pb_ostream_t *stream, const uint8_t *buf, size_t count);
static bool checkreturn encode_ltype(pb_ostream_t *stream, const pb_field_t *field, const void *src, pb_type_t type);
static bool checkreturn encode_array(pb_ostream_t *stream, const pb_field_t *field, const void *pData, size_t count);
static bool checkreturn encode_field(pb_ostream_t *stream, const pb_field_t *field, const void *pData);
static bool checkreturn default_extension_encoder(pb_ostream_t *stream, const pb_extension_t *extension);
static bool checkreturn encode_extension_field(pb_ostream_t *stream, const pb_field_t *field, const void *pData);
static bool checkreturn pb_enc_varint(pb_ostream_t *stream, const pb_field_t *field, const void *src);
static bool checkreturn pb_enc_svarint(pb_ostream_t *stream, const pb_field_t *field, const void *src);
static bool checkreturn pb_enc_fixed32(pb_ostream_t *stream, const pb_field_t *field, const void *src);
static bool checkreturn pb_enc_fixed64(pb_ostream_t *stream, const pb_field_t *field, const void *src);
static bool checkreturn pb_enc_bytes(pb_ostream_t *stream, const pb_field_t *field, const void *src);
static bool checkreturn pb_enc_string(pb_ostream_t *stream, const pb_field_t *field, const void *src);
static bool checkreturn pb_enc_submessage(pb_ostream_t *stream, const pb_field_t *field, const void *src);

/*******************************
 * pb_ostream_t implementation *
 *******************************/

static bool checkreturn buf_write(pb_ostream_t *stream, const uint8_t *buf, size_t count)
{
    uint8_t *dest = (uint8_t*)stream->state;
    stream->state = dest + count;
    
    while (count--)
        *dest++ = *buf++;
    
    return true;
}

pb_ostream_t pb_ostream_from_buffer(uint8_t *buf, size_t bufsize)
{
    pb_ostream_t stream;
#ifdef PB_BUFFER_ONLY
    stream.callback = (void*)1; /* Just a marker value */
#else
    stream.callback = &buf_write;
#endif
    stream.state = buf;
    stream.max_size = bufsize;
    stream.bytes_written = 0;
#ifndef PB_NO_ERRMSG
    stream.errmsg = NULL;
#endif
    return stream;
}

bool checkreturn pb_write(pb_ostream_t *stream, const uint8_t *buf, size_t count)
{
    if (stream->callback != NULL)
    {
        if (stream->bytes_written + count > stream->max_size)
            PB_RETURN_ERROR(stream, "stream full");

#ifdef PB_BUFFER_ONLY
        if (!buf_write(stream, buf, count))
            PB_RETURN_ERROR(stream, "io error");
#else        
        if (!stream->callback(stream, buf, count))
            PB_RETURN_ERROR(stream, "io error");
#endif
    }
    
    stream->bytes_written += count;
    return true;
}

/*************************
 * Encode a single field *
 *************************/

/* Invoke an encoder function based on the ltype of a field. */
static bool checkreturn encode_ltype(pb_ostream_t *stream,
    const pb_field_t *field, const void *src, pb_type_t type)
{
    switch (PB_LTYPE(type))
    {
        case PB_LTYPE_VARINT:       return pb_enc_varint(stream, field, src);
        case PB_LTYPE_SVARINT:      return pb_enc_svarint(stream, field, src);
        case PB_LTYPE_FIXED32:      return pb_enc_fixed32(stream, field, src);
        case PB_LTYPE_FIXED64:      return pb_enc_fixed64(stream, field, src);
        case PB_LTYPE_BYTES:        return pb_enc_bytes(stream, field, src);
        case PB_LTYPE_STRING:       return pb_enc_string(stream, field, src);
        case PB_LTYPE_SUBMESSAGE:   return pb_enc_submessage(stream, field, src);
        
        default: PB_RETURN_ERROR(stream, "invalid field type");
    }
}
    

/* Encode a static array. Handles the size calculations and possible packing. */
static bool checkreturn encode_array(pb_ostream_t *stream, const pb_field_t *field,
                         const void *pData, size_t count)
{
    size_t i;
    const void *p;
    size_t size;
    
    if (count == 0)
        return true;
        
    if (count > field->array_size)
        PB_RETURN_ERROR(stream, "array max size exceeded");
    
    /* We always pack arrays if the datatype allows it. */
    if (PB_LTYPE(field->type) <= PB_LTYPE_LAST_PACKABLE)
    {
        if (!pb_encode_tag(stream, PB_WT_STRING, field->tag))
            return false;
        
        /* Determine the total size of packed array. */
        if (PB_LTYPE(field->type) == PB_LTYPE_FIXED32)
        {
            size = 4 * count;
        }
        else if (PB_LTYPE(field->type) == PB_LTYPE_FIXED64)
        {
            size = 8 * count;
        }
        else
        { 
            pb_ostream_t sizestream = PB_OSTREAM_SIZING;
            p = pData;
            for (i = 0; i < count; i++)
            {
                if (!encode_ltype(&sizestream, field, p, field->type))
                    return false;
                p = (const char*)p + field->data_size;
            }
            size = sizestream.bytes_written;
        }
        
        if (!pb_encode_varint(stream, (uint64_t)size))
            return false;
        
        if (stream->callback == NULL)
            return pb_write(stream, NULL, size); /* Just sizing.. */
        
        /* Write the data */
        p = pData;
        for (i = 0; i < count; i++)
        {
            if (!encode_ltype(stream, field, p, field->type))
                return false;
            p = (const char*)p + field->data_size;
        }
    }
    else
    {
        p = pData;
        for (i = 0; i < count; i++)
        {
            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!encode_ltype(stream, field, p, field->type))
                return false;
            p = (const char*)p + field->data_size;
        }
    }
    
    return true;
}

/* Encode a field with static allocation, i.e. one whose data is stored
 * in the structure itself. */
static bool checkreturn encode_static_field(pb_ostream_t *stream,
    const pb_field_t *field, const void *pData)
{
    const void *pSize;
    bool dummy = true;
    
    if (field->size_offset)
        pSize = (const char*)pData + field->size_offset;
    else
        pSize = &dummy;
    
    switch (PB_HTYPE(field->type))
    {
        case PB_HTYPE_REQUIRED:
            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!encode_ltype(stream, field, pData, field->type))
                return false;
            break;
        
        case PB_HTYPE_OPTIONAL:
            if (*(const bool*)pSize)
            {
                if (!pb_encode_tag_for_field(stream, field))
                    return false;
            
                if (!encode_ltype(stream, field, pData, field->type))
                    return false;
            }
            break;
        
        case PB_HTYPE_REPEATED:
            if (!encode_array(stream, field, pData, *(const size_t*)pSize))
                return false;
            break;
        
        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
    
    return true;
}

/* Encode a field with callback semantics. This means that a user function is
 * called to provide and encode the actual data. */
static bool checkreturn encode_callback_field(pb_ostream_t *stream,
    const pb_field_t *field, const void *pData)
{
    const pb_callback_t *callback = (const pb_callback_t*)pData;
    
#ifdef PB_OLD_CALLBACK_STYLE
    const void *arg = callback->arg;
#else
    void * const *arg = &(callback->arg);
#endif    
    
    if (callback->funcs.encode != NULL)
    {
        if (!callback->funcs.encode(stream, field, arg))
            PB_RETURN_ERROR(stream, "callback error");
    }
    return true;
}

/* Encode a single field of any callback or static type. */
static bool checkreturn encode_field(pb_ostream_t *stream,
    const pb_field_t *field, const void *pData)
{
    switch (PB_ATYPE(field->type))
    {
        case PB_ATYPE_STATIC:
            return encode_static_field(stream, field, pData);
        
        case PB_ATYPE_CALLBACK:
            return encode_callback_field(stream, field, pData);
        
        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
}

/* Default handler for extension fields. Expects to have a pb_field_t
 * pointer in the extension->type->arg field. */
static bool checkreturn default_extension_encoder(pb_ostream_t *stream,
    const pb_extension_t *extension)
{
    const pb_field_t *field = (const pb_field_t*)extension->type->arg;
    return encode_field(stream, field, extension->dest);
}

/* Walk through all the registered extensions and give them a chance
 * to encode themselves. */
static bool checkreturn encode_extension_field(pb_ostream_t *stream,
    const pb_field_t *field, const void *pData)
{
    const pb_extension_t *extension = *(const pb_extension_t* const *)pData;
    UNUSED(field);
    
    while (extension)
    {
        bool status;
        if (extension->type->encode)
            status = extension->type->encode(stream, extension);
        else
            status = default_extension_encoder(stream, extension);

        if (!status)
            return false;
        
        extension = extension->next;
    }
    
    return true;
}

/*********************
 * Encode all fields *
 *********************/

bool checkreturn pb_encode(pb_ostream_t *stream, const pb_field_t fields[], const void *src_struct)
{
    const pb_field_t *field = fields;
    const void *pData = src_struct;
    size_t prev_size = 0;
    
    while (field->tag != 0)
    {
        pData = (const char*)pData + prev_size + field->data_offset;
        prev_size = field->data_size;
        
        /* Special case for static arrays */
        if (PB_ATYPE(field->type) == PB_ATYPE_STATIC &&
            PB_HTYPE(field->type) == PB_HTYPE_REPEATED)
        {
            prev_size *= field->array_size;
        }
        
        if (PB_LTYPE(field->type) == PB_LTYPE_EXTENSION)
        {
            /* Special case for the extension field placeholder */
            if (!encode_extension_field(stream, field, pData))
                return false;
        }
        else
        {
            /* Regular field */
            if (!encode_field(stream, field, pData))
                return false;
        }
    
        field++;
    }
    
    return true;
}

bool pb_encode_delimited(pb_ostream_t *stream, const pb_field_t fields[], const void *src_struct)
{
    return pb_encode_submessage(stream, fields, src_struct);
}

/********************
 * Helper functions *
 ********************/
bool checkreturn pb_encode_varint(pb_ostream_t *stream, uint64_t value)
{
    uint8_t buffer[10];
    size_t i = 0;
    
    if (value == 0)
        return pb_write(stream, (uint8_t*)&value, 1);
    
    while (value)
    {
        buffer[i] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
        i++;
    }
    buffer[i-1] &= 0x7F; /* Unset top bit on last byte */
    
    return pb_write(stream, buffer, i);
}

bool checkreturn pb_encode_svarint(pb_ostream_t *stream, int64_t value)
{
    uint64_t zigzagged;
    if (value < 0)
        zigzagged = (uint64_t)(~(value << 1));
    else
        zigzagged = (uint64_t)(value << 1);
    
    return pb_encode_varint(stream, zigzagged);
}

bool checkreturn pb_encode_fixed32(pb_ostream_t *stream, const void *value)
{
    #ifdef __BIG_ENDIAN__
    const uint8_t *bytes = value;
    uint8_t lebytes[4];
    lebytes[0] = bytes[3];
    lebytes[1] = bytes[2];
    lebytes[2] = bytes[1];
    lebytes[3] = bytes[0];
    return pb_write(stream, lebytes, 4);
    #else
    return pb_write(stream, (const uint8_t*)value, 4);
    #endif
}

bool checkreturn pb_encode_fixed64(pb_ostream_t *stream, const void *value)
{
    #ifdef __BIG_ENDIAN__
    const uint8_t *bytes = value;
    uint8_t lebytes[8];
    lebytes[0] = bytes[7];
    lebytes[1] = bytes[6];
    lebytes[2] = bytes[5];
    lebytes[3] = bytes[4];
    lebytes[4] = bytes[3];
    lebytes[5] = bytes[2];
    lebytes[6] = bytes[1];
    lebytes[7] = bytes[0];
    return pb_write(stream, lebytes, 8);
    #else
    return pb_write(stream, (const uint8_t*)value, 8);
    #endif
}

bool checkreturn pb_encode_tag(pb_ostream_t *stream, pb_wire_type_t wiretype, uint32_t field_number)
{
    uint64_t tag = wiretype | (field_number << 3);
    return pb_encode_varint(stream, tag);
}

bool checkreturn pb_encode_tag_for_field(pb_ostream_t *stream, const pb_field_t *field)
{
    pb_wire_type_t wiretype;
    switch (PB_LTYPE(field->type))
    {
        case PB_LTYPE_VARINT:
        case PB_LTYPE_SVARINT:
            wiretype = PB_WT_VARINT;
            break;
        
        case PB_LTYPE_FIXED32:
            wiretype = PB_WT_32BIT;
            break;
        
        case PB_LTYPE_FIXED64:
            wiretype = PB_WT_64BIT;
            break;
        
        case PB_LTYPE_BYTES:
        case PB_LTYPE_STRING:
        case PB_LTYPE_SUBMESSAGE:
            wiretype = PB_WT_STRING;
            break;
        
        default:
            PB_RETURN_ERROR(stream, "invalid field type");
    }
    
    return pb_encode_tag(stream, wiretype, field->tag);
}

bool checkreturn pb_encode_string(pb_ostream_t *stream, const uint8_t *buffer, size_t size)
{
    if (!pb_encode_varint(stream, (uint64_t)size))
        return false;
    
    return pb_write(stream, buffer, size);
}

bool checkreturn pb_encode_submessage(pb_ostream_t *stream, const pb_field_t fields[], const void *src_struct)
{
    /* First calculate the message size using a non-writing substream. */
    pb_ostream_t substream = PB_OSTREAM_SIZING;
    size_t size;
    bool status;
    
    if (!pb_encode(&substream, fields, src_struct))
        return false;
    
    size = substream.bytes_written;
    
    if (!pb_encode_varint(stream, (uint64_t)size))
        return false;
    
    if (stream->callback == NULL)
        return pb_write(stream, NULL, size); /* Just sizing */
    
    if (stream->bytes_written + size > stream->max_size)
        PB_RETURN_ERROR(stream, "stream full");
        
    /* Use a substream to verify that a callback doesn't write more than
     * what it did the first time. */
    substream.callback = stream->callback;
    substream.state = stream->state;
    substream.max_size = size;
    substream.bytes_written = 0;
#ifndef PB_NO_ERRMSG
    substream.errmsg = NULL;
#endif
    
    status = pb_encode(&substream, fields, src_struct);
    
    stream->bytes_written += substream.bytes_written;
    stream->state = substream.state;
#ifndef PB_NO_ERRMSG
    stream->errmsg = substream.errmsg;
#endif
    
    if (substream.bytes_written != size)
        PB_RETURN_ERROR(stream, "submsg size changed");
    
    return status;
}

/* Field encoders */

bool checkreturn pb_enc_varint(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    uint64_t value = 0;
    
    switch (field->data_size)
    {
        case 1: value = *(const uint8_t*)src; break;
        case 2: value = *(const uint16_t*)src; break;
        case 4: value = *(const uint32_t*)src; break;
        case 8: value = *(const uint64_t*)src; break;
        default: PB_RETURN_ERROR(stream, "invalid data_size");
    }
    
    return pb_encode_varint(stream, value);
}

bool checkreturn pb_enc_svarint(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    int64_t value = 0;
    
    switch (field->data_size)
    {
        case 4: value = *(const int32_t*)src; break;
        case 8: value = *(const int64_t*)src; break;
        default: PB_RETURN_ERROR(stream, "invalid data_size");
    }
    
    return pb_encode_svarint(stream, value);
}

bool checkreturn pb_enc_fixed64(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    UNUSED(field);
    return pb_encode_fixed64(stream, src);
}

bool checkreturn pb_enc_fixed32(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    UNUSED(field);
    return pb_encode_fixed32(stream, src);
}

bool checkreturn pb_enc_bytes(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    const pb_bytes_array_t *bytes = (const pb_bytes_array_t*)src;

    if (bytes->size + offsetof(pb_bytes_array_t, bytes) > field->data_size)
        PB_RETURN_ERROR(stream, "bytes size exceeded");
    
    return pb_encode_string(stream, bytes->bytes, bytes->size);
}

bool checkreturn pb_enc_string(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    /* strnlen() is not always available, so just use a for-loop */
    size_t size = 0;
    const char *p = (const char*)src;
    while (size < field->data_size && *p != '\0')
    {
        size++;
        p++;
    }

    return pb_encode_string(stream, (const uint8_t*)src, size);
}

bool checkreturn pb_enc_submessage(pb_ostream_t *stream, const pb_field_t *field, const void *src)
{
    if (field->ptr == NULL)
        PB_RETURN_ERROR(stream, "invalid field descriptor");
    
    return pb_encode_submessage(stream, (const pb_field_t*)field->ptr, src);
}

