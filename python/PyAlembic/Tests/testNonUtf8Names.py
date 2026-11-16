#-******************************************************************************
# Tests for tolerant, reversible handling of object/property names that are
# not valid UTF-8 (PEP 383 "surrogateescape" conversion, see PyNameUtils.h).
#-******************************************************************************

import os
import shutil
import sys
import unittest

from alembic.Abc import *
from alembic.AbcGeom import *

# the audit tool lives next to the PyAlembic sources
sys.path.insert(
    0, os.path.dirname( os.path.dirname( os.path.abspath( __file__ ) ) ) )
import abc_encoding_audit


# "中文" (zhongwen) encoded in GBK and Big5; both are invalid UTF-8
GBK_NAME = b'gbk_\xd6\xd0\xce\xc4_obj'
BIG5_NAME = b'big5_\xa4\xa4\xa4\xe5_obj'
GBK_PROP = b'prop_\xd6\xd0'
GBK_CHILD = b'child_\xc4\xe3\xba\xc3'   # "你好" in GBK
UTF8_NAME = 'utf8_\u4e2d\u6587_obj'
ASCII_NAME = 'plain_obj'

# distinct byte names that a lossy conversion (e.g. "?") would collapse
# into the same string
COLLIDE_A = b'coll_\x80'
COLLIDE_B = b'coll_\x81'

NONUTF8_FILE = 'testNonUtf8Names.abc'
ROUNDTRIP_FILE = 'testNonUtf8NamesRoundTrip.abc'
NORMAL_FILE = 'testNormalNames.abc'
SCAN_DIR = 'testNonUtf8NamesScan'


def name_bytes( iName ):
    """Original archive bytes of a name read through PyAlembic."""
    return iName.encode( 'utf-8', 'surrogateescape' )


DEFAULT_NAMES = [GBK_NAME, BIG5_NAME, UTF8_NAME, ASCII_NAME,
                 COLLIDE_A, COLLIDE_B]


def build_nonutf8_archive( iFileName, iNames=None, iChildName=None,
                           iPropName=None ):
    """Archive mixing GBK/Big5/UTF-8/ASCII object names, a nested child
    and a non-UTF-8 property name.  Names default to the original byte
    strings; passing back the str names read from such an archive must
    produce a byte-for-byte identical file."""
    names = iNames if iNames is not None else DEFAULT_NAMES
    childName = iChildName if iChildName is not None else GBK_CHILD
    propName = iPropName if iPropName is not None else GBK_PROP

    archive = OArchive( iFileName )
    top = archive.getTop()

    gbk = OObject( top, names[0] )
    big5 = OObject( top, names[1] )
    utf8 = OObject( top, names[2] )
    asciiObj = OObject( top, names[3] )
    collA = OObject( top, names[4] )
    collB = OObject( top, names[5] )

    OObject( gbk, childName )

    prop = OStringProperty( gbk.getProperties(), propName )
    prop.setValue( 'gbk prop value' )
    count = OInt32Property( asciiObj.getProperties(), 'count' )
    count.setValue( 42 )


def build_normal_archive( iFileName ):
    """Well-formed archive: ASCII and valid UTF-8 names only."""
    archive = OArchive( iFileName )
    top = archive.getTop()
    xform = OXform( top, 'myxform' )
    mesh = OObject( xform, UTF8_NAME )
    prop = OStringProperty( mesh.getProperties(), 'myprop' )
    prop.setValue( 'myvalue' )
    count = OInt32Property( mesh.getProperties(), 'count' )
    count.setValue( 42 )


class NonUtf8NamesTest( unittest.TestCase ):

    @classmethod
    def setUpClass( cls ):
        build_nonutf8_archive( NONUTF8_FILE )
        build_normal_archive( NORMAL_FILE )

    @classmethod
    def tearDownClass( cls ):
        for path in ( NONUTF8_FILE, ROUNDTRIP_FILE, NORMAL_FILE ):
            if os.path.exists( path ):
                os.remove( path )
        if os.path.isdir( SCAN_DIR ):
            shutil.rmtree( SCAN_DIR )

    #-*************************************************************************
    def testReadNonUtf8Names( self ):
        """A cache with non-UTF-8 names opens and every object is readable;
        every name converts back to its exact original bytes."""
        archive = IArchive( NONUTF8_FILE )
        top = archive.getTop()

        self.assertEqual( top.getNumChildren(), 6 )

        expected = [GBK_NAME, BIG5_NAME, UTF8_NAME.encode( 'utf-8' ),
                    ASCII_NAME.encode( 'utf-8' ), COLLIDE_A, COLLIDE_B]
        for i in range( top.getNumChildren() ):
            child = top.getChild( i )
            # must not raise
            name = child.getName()
            fullName = child.getFullName()
            headerName = child.getHeader().getName()
            self.assertEqual( name_bytes( name ), expected[i] )
            self.assertEqual( name_bytes( headerName ), expected[i] )
            self.assertEqual( name_bytes( fullName ),
                              b'/' + expected[i] )

        # valid UTF-8 and ASCII names are returned exactly as before
        names = [top.getChild( i ).getName() for i in range( 6 )]
        self.assertEqual( names[2], UTF8_NAME )
        self.assertEqual( names[3], ASCII_NAME )

        # the non-UTF-8 names carry their original bytes as lone surrogates
        self.assertEqual( names[0],
                          GBK_NAME.decode( 'utf-8', 'surrogateescape' ) )

        # nested child with non-UTF-8 name and full path
        gbkObj = top.getChild( names[0] )
        self.assertEqual( gbkObj.getNumChildren(), 1 )
        nested = gbkObj.getChild( 0 )
        self.assertEqual( name_bytes( nested.getName() ), GBK_CHILD )
        self.assertEqual( name_bytes( nested.getFullName() ),
                          b'/' + GBK_NAME + b'/' + GBK_CHILD )

        # non-UTF-8 property name reads back and restores its bytes
        props = gbkObj.getProperties()
        self.assertEqual( props.getNumProperties(), 1 )
        propHeader = props.getPropertyHeader( 0 )
        self.assertEqual( name_bytes( propHeader.getName() ), GBK_PROP )
        prop = props.getProperty( 0 )
        self.assertEqual( name_bytes( prop.getName() ), GBK_PROP )
        self.assertEqual( prop.getValue(), 'gbk prop value' )

        # looking objects and properties up by their converted names works
        self.assertTrue( top.getChild( names[0] ).valid() )
        self.assertTrue(
            props.getProperty( propHeader.getName() ).valid() )

    #-*************************************************************************
    def testRoundTripWritePreservesBytes( self ):
        """Names read from a non-UTF-8 cache write back with their original
        bytes; the rewritten file is byte-for-byte identical."""
        src = IArchive( NONUTF8_FILE )
        top = src.getTop()
        names = [top.getChild( i ).getName()
                 for i in range( top.getNumChildren() )]
        gbkChildName = top.getChild( 0 ).getChild( 0 ).getName()
        propName = top.getChild( 0 ).getProperties() \
                                  .getPropertyHeader( 0 ).getName()
        del src, top

        # rewrite with the converted str names through the exact same
        # write sequence as the original file
        build_nonutf8_archive( ROUNDTRIP_FILE, names, gbkChildName,
                               propName )

        # re-read and verify every name restores the original bytes
        check = IArchive( ROUNDTRIP_FILE )
        checkTop = check.getTop()
        self.assertEqual( checkTop.getNumChildren(), 6 )
        expected = [GBK_NAME, BIG5_NAME, UTF8_NAME.encode( 'utf-8' ),
                    ASCII_NAME.encode( 'utf-8' ), COLLIDE_A, COLLIDE_B]
        for i in range( 6 ):
            self.assertEqual(
                name_bytes( checkTop.getChild( i ).getName() ),
                expected[i] )
        self.assertEqual(
            name_bytes( checkTop.getChild( 0 ).getChild( 0 ).getName() ),
            GBK_CHILD )
        self.assertEqual(
            name_bytes( checkTop.getChild( 0 ).getProperties()
                        .getPropertyHeader( 0 ).getName() ),
            GBK_PROP )
        del check, checkTop

        # archive-level checksum safety: identical bytes on disk
        with open( NONUTF8_FILE, 'rb' ) as f:
            original = f.read()
        with open( ROUNDTRIP_FILE, 'rb' ) as f:
            rewritten = f.read()
        self.assertEqual( original, rewritten )

    #-*************************************************************************
    def testNormalFileUnchanged( self ):
        """A well-formed file reads back field by field exactly as before."""
        archive = IArchive( NORMAL_FILE )
        top = archive.getTop()

        self.assertEqual( top.getName(), 'ABC' )
        self.assertEqual( top.getFullName(), '/' )
        self.assertEqual( top.getNumChildren(), 1 )

        xform = top.getChild( 0 )
        self.assertEqual( xform.getName(), 'myxform' )
        self.assertEqual( xform.getFullName(), '/myxform' )
        self.assertEqual( xform.getNumChildren(), 1 )
        self.assertEqual( xform.getMetaData().get( 'schemaObjTitle' ),
                          'AbcGeom_Xform_v3:.xform' )

        mesh = xform.getChild( 0 )
        self.assertEqual( mesh.getName(), UTF8_NAME )
        self.assertEqual( mesh.getFullName(), '/myxform/' + UTF8_NAME )
        self.assertEqual( mesh.getHeader().getName(), UTF8_NAME )

        props = mesh.getProperties()
        self.assertEqual( props.getNumProperties(), 2 )
        self.assertEqual( props.getPropertyHeader( 0 ).getName(), 'myprop' )
        self.assertEqual( props.getPropertyHeader( 1 ).getName(), 'count' )
        self.assertEqual( props.getProperty( 'myprop' ).getName(),
                          'myprop' )
        self.assertEqual( IStringProperty( props, 'myprop' ).getValue(),
                          'myvalue' )
        self.assertEqual( IInt32Property( props, 'count' ).getValue(), 42 )

    #-*************************************************************************
    def testNoCollisionsAfterConversion( self ):
        """Names that differ only in non-UTF-8 bytes stay distinct after
        conversion; true duplicates are still rejected."""
        archive = IArchive( NONUTF8_FILE )
        top = archive.getTop()
        nameA = COLLIDE_A.decode( 'utf-8', 'surrogateescape' )
        nameB = COLLIDE_B.decode( 'utf-8', 'surrogateescape' )
        self.assertNotEqual( nameA, nameB )

        childA = top.getChild( nameA )
        childB = top.getChild( nameB )
        self.assertTrue( childA.valid() )
        self.assertTrue( childB.valid() )
        self.assertEqual( name_bytes( childA.getName() ), COLLIDE_A )
        self.assertEqual( name_bytes( childB.getName() ), COLLIDE_B )

        # writing a genuine duplicate name is still an error
        out = OArchive( ROUNDTRIP_FILE )
        outTop = out.getTop()
        OObject( outTop, nameA )
        self.assertRaises( Exception, OObject, outTop, nameA )
        del out, outTop
        os.remove( ROUNDTRIP_FILE )

    #-*************************************************************************
    def testScanAndManifest( self ):
        """Batch scan finds affected archives and exports a manifest."""
        nested = os.path.join( SCAN_DIR, 'nested' )
        os.makedirs( nested )
        shutil.copy( NONUTF8_FILE,
                     os.path.join( nested, 'vendor_a.abc' ) )
        shutil.copy( NORMAL_FILE,
                     os.path.join( SCAN_DIR, 'vendor_b.abc' ) )

        entries, failed = abc_encoding_audit.scan_paths( [SCAN_DIR] )
        self.assertEqual( failed, [] )

        affected = set( e['file'] for e in entries )
        self.assertEqual( affected,
                          {os.path.join( nested, 'vendor_a.abc' )} )

        paths = set( e['object_path'] for e in entries )
        expectedPaths = {
            '/' + GBK_NAME.decode( 'utf-8', 'surrogateescape' ),
            '/' + BIG5_NAME.decode( 'utf-8', 'surrogateescape' ),
            '/' + GBK_NAME.decode( 'utf-8', 'surrogateescape' ) + '/'
                + GBK_CHILD.decode( 'utf-8', 'surrogateescape' ),
            '/' + COLLIDE_A.decode( 'utf-8', 'surrogateescape' ),
            '/' + COLLIDE_B.decode( 'utf-8', 'surrogateescape' ),
        }
        self.assertEqual( paths, expectedPaths )

        # original bytes are recoverable from the manifest
        byHex = {}
        for e in entries:
            byHex.setdefault( e['name_bytes_hex'], e )
        self.assertIn( GBK_NAME.hex(), byHex )
        self.assertIn( BIG5_NAME.hex(), byHex )
        self.assertEqual(
            abc_encoding_audit.name_to_bytes(
                byHex[GBK_NAME.hex()]['name'] ), GBK_NAME )

        # manifest export for the vendors
        csvPath = os.path.join( SCAN_DIR, 'manifest.csv' )
        jsonPath = os.path.join( SCAN_DIR, 'manifest.json' )
        abc_encoding_audit.write_csv( entries, csvPath )
        abc_encoding_audit.write_json( entries, jsonPath )

        with open( csvPath, encoding='utf-8' ) as f:
            csvText = f.read()
        self.assertIn( 'vendor_a.abc', csvText )
        self.assertIn( GBK_NAME.hex(), csvText )
        self.assertNotIn( 'vendor_b.abc', csvText )

        import json
        with open( jsonPath, encoding='ascii' ) as f:
            manifest = json.load( f )
        self.assertEqual( len( manifest ), len( entries ) )
        self.assertEqual(
            {e['file'] for e in manifest}, affected )

        # a clean tree produces an empty manifest
        cleanDir = os.path.join( SCAN_DIR, 'clean' )
        os.makedirs( cleanDir )
        shutil.copy( NORMAL_FILE, os.path.join( cleanDir, 'clean.abc' ) )
        cleanEntries, cleanFailed = abc_encoding_audit.scan_paths(
            [cleanDir] )
        self.assertEqual( cleanEntries, [] )
        self.assertEqual( cleanFailed, [] )


if __name__ == '__main__':
    unittest.main()
