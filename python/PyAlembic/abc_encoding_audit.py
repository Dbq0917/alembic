"""Scan Alembic archives for object names that are not valid UTF-8.

PyAlembic decodes names from an archive as UTF-8 with the PEP 383
"surrogateescape" error handler: every byte that is not valid UTF-8 shows
up as a lone surrogate character (U+DC80..U+DCFF) in the returned str.
This module uses that rule to detect archives written by tools running
with a non-UTF-8 local encoding (GBK, Big5, ...) and to export a manifest
that can be handed back to the content owners for cleanup.

Usage:

    python abc_encoding_audit.py ROOT [ROOT ...] [--csv OUT.csv]
                                                 [--json OUT.json]

Each ROOT may be an .abc file or a directory that is searched
recursively for .abc files.  A summary is printed to stdout; the
optional manifest files list every offending object.
"""

import argparse
import csv
import json
import os
import sys


def has_non_utf8( iName ):
    """True if a name read by PyAlembic contains bytes that were not
    valid UTF-8 in the archive (PEP 383 lone surrogates U+DC80..U+DCFF)."""
    return any( 0xDC80 <= ord( c ) <= 0xDCFF for c in iName )


def name_to_bytes( iName ):
    """Recover the exact bytes stored in the archive for a name."""
    return iName.encode( 'utf-8', 'surrogateescape' )


def _walk_object( iObj, oEntries, iFile ):
    for i in range( iObj.getNumChildren() ):
        child = iObj.getChild( i )
        name = child.getName()
        if has_non_utf8( name ):
            oEntries.append( {
                'file': iFile,
                'object_path': child.getFullName(),
                'name': name,
                'name_bytes_hex': name_to_bytes( name ).hex(),
            } )
        _walk_object( child, oEntries, iFile )


def scan_archive( iFile ):
    """Return a list of manifest entries for one archive.  Raises whatever
    the archive reader raises if the file cannot be opened at all."""
    from alembic import Abc

    entries = []
    archive = Abc.IArchive( iFile )
    _walk_object( archive.getTop(), entries, iFile )
    return entries


def find_archives( iRoots ):
    """Yield every .abc file under the given files/directories."""
    for root in iRoots:
        if os.path.isfile( root ):
            yield root
        elif os.path.isdir( root ):
            for dirpath, _dirnames, filenames in os.walk( root ):
                for filename in sorted( filenames ):
                    if filename.lower().endswith( '.abc' ):
                        yield os.path.join( dirpath, filename )
        else:
            print( 'warning: no such file or directory: %s' % root,
                   file=sys.stderr )


def scan_paths( iRoots, iVerbose=False ):
    """Scan every archive found under iRoots.

    Returns a tuple (entries, failed) where entries is the flat manifest
    list and failed is the list of files that could not be opened."""
    entries = []
    failed = []
    for path in find_archives( iRoots ):
        try:
            found = scan_archive( path )
        except Exception as e:
            print( 'warning: cannot scan %s: %s' % ( path, e ),
                   file=sys.stderr )
            failed.append( path )
            continue
        if found and iVerbose:
            print( '%s: %d non-UTF-8 object name(s)' % ( path,
                                                         len( found ) ) )
        entries.extend( found )
    return entries, failed


MANIFEST_FIELDS = ( 'file', 'object_path', 'name', 'name_bytes_hex' )


def write_csv( iEntries, oFileName ):
    """Export the manifest as CSV (UTF-8, undecodable names shown with
    backslash escapes; the exact original bytes are in name_bytes_hex)."""
    with open( oFileName, 'w', newline='', encoding='utf-8',
               errors='backslashreplace' ) as csvfile:
        writer = csv.DictWriter( csvfile, fieldnames=MANIFEST_FIELDS )
        writer.writeheader()
        for entry in iEntries:
            writer.writerow( entry )


def write_json( iEntries, oFileName ):
    """Export the manifest as JSON.  ensure_ascii keeps lone surrogates
    readable (\\udcXX escapes) so the file is always valid ASCII JSON."""
    with open( oFileName, 'w', encoding='ascii' ) as jsonfile:
        json.dump( iEntries, jsonfile, ensure_ascii=True, indent=2,
                   sort_keys=True )
        jsonfile.write( '\n' )


def main( iArgv=None ):
    parser = argparse.ArgumentParser(
        description='Scan Alembic archives for non-UTF-8 object names '
                    'and export a manifest for cleanup.' )
    parser.add_argument( 'roots', nargs='+', metavar='ROOT',
                         help='.abc file or directory to scan recursively' )
    parser.add_argument( '--csv', metavar='OUT.csv',
                         help='write the manifest as CSV' )
    parser.add_argument( '--json', metavar='OUT.json',
                         help='write the manifest as JSON' )
    parser.add_argument( '-q', '--quiet', action='store_true',
                         help='only print the final summary' )
    args = parser.parse_args( iArgv )

    entries, failed = scan_paths( args.roots, iVerbose=not args.quiet )

    if args.csv:
        write_csv( entries, args.csv )
    if args.json:
        write_json( entries, args.json )

    affected = sorted( set( e['file'] for e in entries ) )
    print( 'scanned archives with non-UTF-8 names: %d' % len( affected ) )
    print( 'non-UTF-8 object names found: %d' % len( entries ) )
    if failed:
        print( 'archives that could not be opened: %d' % len( failed ) )
    for path in affected:
        print( '  affected: %s' % path )

    return 0


if __name__ == '__main__':
    sys.exit( main() )
