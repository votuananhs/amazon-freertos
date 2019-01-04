/*
 * Amazon FreeRTOS System Initialization
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

 /**
  * @file aws_iot_serializer_tinycbor_decoder.c
  * @brief Implements APIs to parse and decode data from CBOR format. The file relies on tiny
  * CBOR library to parse and decode data from CBOR format. Supports all major data types within
  * the CBOR format.
  * The file implements the decoder interface in aws_iot_serialize.h.
  */


#include "FreeRTOS.h"
#include "aws_iot_serializer.h"
#include "cbor.h"

#define _noCborError( error )                              ( ( error == CborNoError ) )

#define _undefinedType( type )                             ( ( type == AWS_IOT_SERIALIZER_UNDEFINED ) )

#define _castDecoderObjectToCborValue( pDecoderObject )    (  & ( ( ( _cborDecoderInternal_t * )(  pDecoderObject->pHandle ) )->cborValue ) )

#define _castDecoderIteratorToCborValue( iterator )        ( _castDecoderObjectToCborValue ( ( ( AwsIotSerializerDecoderObject_t *) iterator ) ) )

static AwsIotSerializerError_t _init(AwsIotSerializerDecoderObject_t * pDecoderObject,
    uint8_t * pDataBuffer,
    size_t maxSize);
static AwsIotSerializerError_t _get(AwsIotSerializerDecoderIterator_t iterator,
    AwsIotSerializerDecoderObject_t * pValueObject);

static AwsIotSerializerError_t _find(AwsIotSerializerDecoderObject_t* pDecoderObject,
    const char * pKey,
    AwsIotSerializerDecoderObject_t * pValueObject);
static AwsIotSerializerError_t  _stepIn( AwsIotSerializerDecoderObject_t * pDecoderObject,
                                       AwsIotSerializerDecoderIterator_t * pIterator);
static AwsIotSerializerError_t  _stepOut( AwsIotSerializerDecoderIterator_t iterator,
                                          AwsIotSerializerDecoderObject_t * pDecoderObject );

static AwsIotSerializerError_t _next( AwsIotSerializerDecoderIterator_t iterator);

static bool _isEndOfContainer( AwsIotSerializerDecoderIterator_t iterator);

static AwsIotSerializerError_t _destroy( AwsIotSerializerDecoderObject_t * pDecoderObject );

static void _print(uint8_t * pDataBuffer,
    size_t dataSize);

AwsIotSerializerDecodeInterface_t _AwsIotSerializerCborDecoder =
{
    .print = _print,
    .init = _init,
    .get = _get,
    .find = _find,
    .stepIn = _stepIn,
    .stepOut = _stepOut,
    .next = _next,
    .isEndOfContainer = _isEndOfContainer,
    .destroy = _destroy
};

typedef struct _cborDecoderInternal
{
   CborValue cborValue;
   bool isOutermost;
} _cborDecoderInternal_t;

/*-----------------------------------------------------------*/

static AwsIotSerializerDataType_t _toSerializerType(CborType type)
{
    switch (type)
    {
    case CborIntegerType:

        return AWS_IOT_SERIALIZER_SCALAR_SIGNED_INT;

    case CborBooleanType:

        return AWS_IOT_SERIALIZER_SCALAR_BOOL;

    case CborByteStringType:

        return AWS_IOT_SERIALIZER_SCALAR_BYTE_STRING;

    case CborTextStringType:

        return AWS_IOT_SERIALIZER_SCALAR_TEXT_STRING;

    case CborArrayType:

        return AWS_IOT_SERIALIZER_CONTAINER_ARRAY;

    case CborMapType:

        return AWS_IOT_SERIALIZER_CONTAINER_MAP;

    default:

        return AWS_IOT_SERIALIZER_UNDEFINED;
    }
}

/*-----------------------------------------------------------*/

static AwsIotSerializerError_t _createDecoderObject(CborValue * pCborValue,
    AwsIotSerializerDecoderObject_t * pValueObject, bool isOuterMost )
{
    AwsIotSerializerError_t returnError = AWS_IOT_SERIALIZER_SUCCESS;
    AwsIotSerializerDataType_t dataType = _toSerializerType(cbor_value_get_type(pCborValue));
    CborError cborError;
    _cborDecoderInternal_t *pContainerDecoder;


    if (_undefinedType(dataType))
    {
        returnError = AWS_IOT_SERIALIZER_UNDEFINED_TYPE;
    }
    else
    {
        pValueObject->type = dataType;
    }

    if (returnError == AWS_IOT_SERIALIZER_SUCCESS)
    {
        switch (pValueObject->type)
        {
        case AWS_IOT_SERIALIZER_SCALAR_SIGNED_INT:
        {
            int i = 0;
            cbor_value_get_int(pCborValue, &i);
            pValueObject->value = (AwsIotSerializerScalarValue_t) {
                .signedInt = i
            };
            break;
        }

        case AWS_IOT_SERIALIZER_SCALAR_TEXT_STRING:
        {
            size_t length;


            /* user doesn't provide the buffer */
            if (pValueObject->value.pString == NULL)
            {

                if( cbor_value_is_length_known( pCborValue ) )
                {
                    /*
                     * If the length of the CBOR string is known, then its
                     * a finite length string. We store the pointer to the start of the string
                     * and length as the length of the string.
                     *
                     */
                    cborError = cbor_value_get_string_length( pCborValue, &length );
                    if( _noCborError( cborError ) )
                    {
                        pValueObject->value.pString =  ( uint8_t* ) cbor_value_get_next_byte( pCborValue ) - length;
                        pValueObject->value.stringLength = length;
                    }
                    else
                    {
                        returnError = AWS_IOT_SERIALIZER_INTERNAL_FAILURE;
                    }
                }
                else
                {
                    /*
                     * This is an infinite length string.
                     * Calculate and save required length to pValueObject, leave the pString pointer
                     * as null so user can allocate the required buffer.
                     */
                    cbor_value_calculate_string_length(pCborValue, &length);
                    pValueObject->value.stringLength = length;
                    returnError = AWS_IOT_SERIALIZER_OUT_OF_MEMORY;
                }
            }
            else /* user provides the buffer */
            {
                /* copy the string to the buffer */
                cbor_value_copy_text_string(pCborValue, ( char * ) pValueObject->value.pString, &length, NULL);
                pValueObject->value.stringLength = length;
            }

            break;
        }

        case AWS_IOT_SERIALIZER_CONTAINER_MAP:
        case AWS_IOT_SERIALIZER_CONTAINER_ARRAY:
        {
            pContainerDecoder = pvPortMalloc( sizeof(_cborDecoderInternal_t));
            if( pContainerDecoder != NULL )
            {
                pContainerDecoder->cborValue = *pCborValue;
                pContainerDecoder->isOutermost = isOuterMost;
                pValueObject->pHandle = ( void *) pContainerDecoder;
            }
            else
            {
                returnError = AWS_IOT_SERIALIZER_OUT_OF_MEMORY;
            }
            break;
        }

        default:
            returnError = AWS_IOT_SERIALIZER_UNDEFINED_TYPE;
        }
    }

    return returnError;
}

/*-----------------------------------------------------------*/

static AwsIotSerializerError_t _init(AwsIotSerializerDecoderObject_t * pDecoderObject,
    uint8_t * pDataBuffer,
    size_t maxSize)
{
    CborParser *pCborParser = pvPortMalloc(sizeof(CborValue));
    AwsIotSerializerError_t returnError = AWS_IOT_SERIALIZER_SUCCESS;
    CborError cborError;
    CborValue cborValue = { 0 };

    cborError = cbor_parser_init(
        pDataBuffer,
        maxSize,
        0,
        pCborParser,
        &cborValue );

    if (_noCborError(cborError))
    {
        _createDecoderObject(&cborValue, pDecoderObject, true);
    }
    else
    {
        vPortFree( pCborParser );
        returnError = AWS_IOT_SERIALIZER_INTERNAL_FAILURE;
    }

    return returnError;
}

/*-----------------------------------------------------------*/

static AwsIotSerializerError_t _get(AwsIotSerializerDecoderIterator_t iterator,
    AwsIotSerializerDecoderObject_t * pValueObject)
{
    CborValue * pCborValue = _castDecoderIteratorToCborValue( iterator );

    return _createDecoderObject( pCborValue, pValueObject, false );
}

/*-----------------------------------------------------------*/

static AwsIotSerializerError_t _find(AwsIotSerializerDecoderObject_t* pDecoderObject,
    const char * pKey,
    AwsIotSerializerDecoderObject_t * pValueObject)
{
    AwsIotSerializerError_t error = AWS_IOT_SERIALIZER_INTERNAL_FAILURE;
    CborValue * pCborValue = _castDecoderObjectToCborValue( pDecoderObject );
    CborError cborError;
    CborValue newCborValue = { 0 };

    cborError = cbor_value_map_find_value(
        pCborValue,
        pKey,
        &newCborValue );

    if( _noCborError( cborError ) )
    {
        error = _createDecoderObject( &newCborValue, pValueObject, false);
    }

    return error;
}


static AwsIotSerializerError_t  _stepIn( AwsIotSerializerDecoderObject_t * pDecoderObject,
                                       AwsIotSerializerDecoderIterator_t * pIterator)
{

    AwsIotSerializerError_t error = AWS_IOT_SERIALIZER_INTERNAL_FAILURE;
    CborError cborError;
    CborValue *pCborValue = _castDecoderObjectToCborValue( pDecoderObject );
    CborValue innerValue = { 0 };
    AwsIotSerializerDecoderObject_t* pInnerObject = pvPortMalloc( sizeof( AwsIotSerializerDecoderObject_t ) );

    cborError = cbor_value_enter_container(
            pCborValue,
            &innerValue );

    if( _noCborError( cborError ) )
    {
        error = _createDecoderObject( &innerValue, pInnerObject, false);
        pInnerObject->type = pDecoderObject->type;
        *pIterator = ( AwsIotSerializerDecoderIterator_t ) pInnerObject;
        error = AWS_IOT_SERIALIZER_SUCCESS;
    }
    else
    {
        vPortFree( pInnerObject );
    }

    return error;
}

static AwsIotSerializerError_t  _stepOut( AwsIotSerializerDecoderIterator_t iterator,
                                          AwsIotSerializerDecoderObject_t * pDecoderObject )
{
    AwsIotSerializerError_t error = AWS_IOT_SERIALIZER_INTERNAL_FAILURE;
    CborError cborError;
    AwsIotSerializerDecoderObject_t *pInnerObject = ( AwsIotSerializerDecoderObject_t *) iterator;
    CborValue *pOuterValue = _castDecoderObjectToCborValue( pDecoderObject );
    CborValue *pInnerValue = _castDecoderObjectToCborValue( pInnerObject );

    cborError = cbor_value_leave_container(
            pOuterValue,
            pInnerValue );

    if( _noCborError( cborError ) )
    {
        vPortFree( pInnerObject->pHandle );
        vPortFree( pInnerObject );
        error = AWS_IOT_SERIALIZER_SUCCESS;
    }

    return error;
}

static AwsIotSerializerError_t _next( AwsIotSerializerDecoderIterator_t iterator)
{
    AwsIotSerializerError_t error = AWS_IOT_SERIALIZER_INTERNAL_FAILURE;
    CborError cborError;
    CborValue* pCborValue = _castDecoderIteratorToCborValue( iterator );
    cborError = cbor_value_advance( pCborValue );

    if( _noCborError( cborError ) )
    {
        error = AWS_IOT_SERIALIZER_SUCCESS;
    }

     return error;
}

static bool _isEndOfContainer( AwsIotSerializerDecoderIterator_t iterator)
{
    CborValue* pCborValue = _castDecoderIteratorToCborValue( iterator );
    return cbor_value_at_end( pCborValue );
}

static AwsIotSerializerError_t _destroy( AwsIotSerializerDecoderObject_t * pDecoderObject )
{
    _cborDecoderInternal_t* pInternalCborDecoder = ( _cborDecoderInternal_t *) ( pDecoderObject->pHandle );
    AwsIotSerializerError_t error = AWS_IOT_SERIALIZER_INVALID_INPUT;
    if( pInternalCborDecoder != NULL )
    {
        if( pInternalCborDecoder->isOutermost )
        {
            vPortFree( ( void* ) ( pInternalCborDecoder->cborValue.parser ) );
        }
        vPortFree( pInternalCborDecoder );
        pDecoderObject->pHandle = NULL;
        error = AWS_IOT_SERIALIZER_SUCCESS;
    }

    return error;
}
/*-----------------------------------------------------------*/

static void _print(uint8_t * pDataBuffer,
    size_t dataSize)
{
    CborParser cborParser;
    CborValue cborValue;

    cbor_parser_init(
        pDataBuffer,
        dataSize,
        0,
        &cborParser,
        &cborValue);

    /* output to standard out */
    cbor_value_to_pretty(stdout, &cborValue);
}
