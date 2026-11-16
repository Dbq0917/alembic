//
// Helpers for converting object and property names between the raw byte
// strings stored in an Alembic archive and Python str objects.
//
// Archives in the wild may contain names that are not valid UTF-8 (for
// example names written in a local encoding such as GBK or Big5 by a DCC
// running on a machine with a non-UTF-8 locale).  The stock std::string
// converters in Boost.Python decode and encode UTF-8 strictly, so a single
// such name makes the whole archive unreadable from Python.
//
// The conversion rule used here is PEP 383 ("surrogateescape"):
//
//   * decode (C++ -> Python): UTF-8, every byte that is not part of a
//     valid UTF-8 sequence is mapped to the lone surrogate U+DC00+byte
//     (U+DC80..U+DCFF).
//   * encode (Python -> C++): UTF-8, every lone surrogate U+DC80..U+DCFF
//     is mapped back to the single byte (code point - 0xDC00).
//
// The rule is total (any byte sequence can be read), injective (distinct
// byte sequences give distinct strings, so names can never collide after
// conversion) and a round trip reproduces the original bytes exactly, so
// names read from an archive can be written back byte-for-byte without
// normalizing anything to UTF-8.  Names that are valid UTF-8 convert
// exactly as before, so well-formed files are unaffected.
//

#ifndef PyAlembic_PyNameUtils_h_
#define PyAlembic_PyNameUtils_h_

#include <Foundation.h>

// Convert a raw byte string from an archive into a Python str using UTF-8
// with the "surrogateescape" error handler.  Never raises for malformed
// input bytes.
inline boost::python::object nameToPython( const std::string &iName )
{
    PyObject *str = PyUnicode_DecodeUTF8( iName.data(),
                                          ( Py_ssize_t )iName.size(),
                                          "surrogateescape" );
    if ( str == NULL )
    {
        boost::python::throw_error_already_set();
    }
    return boost::python::object( boost::python::handle<>( str ) );
}

// nameToPython adapter for any bound class with a getName() method, e.g.
// IBasePropertyT / OBasePropertyT instantiations and geom params
template <class T>
inline boost::python::object getNameToPython( T &iNamed )
{
    return nameToPython( iNamed.getName() );
}

// From-python converter for std::string.  Registered with a higher priority
// than the Boost.Python built-in converter so that every std::string
// argument (object names, property names, ...) accepts:
//
//   * str   -> encoded as UTF-8 with "surrogateescape", so names that were
//              read back with lone surrogates reproduce their original bytes
//   * bytes -> passed through unchanged (existing behavior)
//
// Valid UTF-8 strings encode exactly as with the built-in converter.
struct string_from_python_name
{
    static void *convertible( PyObject *obj_ptr )
    {
        if ( PyUnicode_Check( obj_ptr ) || PyBytes_Check( obj_ptr ) )
        {
            return obj_ptr;
        }
        return 0;
    }

    static void construct( PyObject *obj_ptr,
        boost::python::converter::rvalue_from_python_stage1_data *data )
    {
        PyObject *bytes = NULL;
        if ( PyUnicode_Check( obj_ptr ) )
        {
            bytes = PyUnicode_AsEncodedString( obj_ptr, "utf-8",
                                               "surrogateescape" );
            if ( bytes == NULL )
            {
                boost::python::throw_error_already_set();
            }
        }
        else
        {
            bytes = obj_ptr;
            Py_INCREF( bytes );
        }

        void *storage =
            ( ( boost::python::converter::rvalue_from_python_storage<
                    std::string > * ) data )->storage.bytes;

        new ( storage ) std::string( PyBytes_AS_STRING( bytes ),
                                     ( size_t )PyBytes_GET_SIZE( bytes ) );
        data->convertible = storage;
        Py_DECREF( bytes );
    }
};

// Register the from-python converter above.  Boost.Python tries rvalue
// from-python converters most-recently-registered first, so this takes
// precedence over the built-in strict std::string converter.
inline void registerNameConverters()
{
    boost::python::converter::registry::insert(
        &string_from_python_name::convertible,
        &string_from_python_name::construct,
        boost::python::type_id<std::string>() );
}

#endif
