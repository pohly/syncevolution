/****************************************************************************
**
** DCOP Stub Definition created by dcopidl2cpp from KNotesIface.kidl
**
** WARNING! All changes made in this file will be lost!
**
*****************************************************************************/

#ifndef __KNOTESIFACE_STUB__
#define __KNOTESIFACE_STUB__

#include <dcopstub.h>
#include <dcopobject.h>
#include <tqmap.h>
#include <tqdatetime.h>
#include <tqstring.h>


class KNotesIface_stub : virtual public DCOPStub
{
public:
    KNotesIface_stub( const TQCString& app, const TQCString& id );
    KNotesIface_stub( DCOPClient* client, const TQCString& app, const TQCString& id );
    explicit KNotesIface_stub( const DCOPRef& ref );
    virtual TQString newNote( const TQString& name, const TQString& text );
    virtual TQString newNoteFromClipboard( const TQString& name );
    virtual ASYNC killNote( const TQString& noteId );
    virtual ASYNC killNote( const TQString& noteId, bool force );
    virtual TQMap<TQString,TQString> notes();
    virtual ASYNC setName( const TQString& noteId, const TQString& newName );
    virtual ASYNC setText( const TQString& noteId, const TQString& newText );
    virtual TQString name( const TQString& noteId );
    virtual TQString text( const TQString& noteId );
    virtual int getRevision( const TQString& noteId );
    virtual TQDateTime getLastModified( const TQString& noteId );
protected:
    KNotesIface_stub() : DCOPStub( never_use ) {}
};


#endif
