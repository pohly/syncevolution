/****************************************************************************
**
** DCOP Stub Definition created by dcopidl2cpp from KNotesIface.kidl
**
** WARNING! All changes made in this file will be lost!
**
** $Id: KNotesIface_stub.h,v 1.2 2016/09/01 10:40:05 emanoil Exp $
**
*****************************************************************************/

#ifndef __KNOTESIFACE_STUB__
#define __KNOTESIFACE_STUB__

#include <dcopstub.h>
#include <dcopobject.h>
#include <tqmap.h>
#include <tqstring.h>


class KNotesIface_stub : virtual public DCOPStub
{
public:
    KNotesIface_stub( const TQCString& app, const TQCString& id );
    KNotesIface_stub( DCOPClient* client, const TQCString& app, const TQCString& id );
    explicit KNotesIface_stub( const DCOPRef& ref );
    virtual TQString newNote( const TQString& name, const TQString& text );
    virtual TQString newNoteFromClipboard( const TQString& name );
    virtual ASYNC showNote( const TQString& noteId );
    virtual ASYNC hideNote( const TQString& noteId );
    virtual ASYNC killNote( const TQString& noteId );
    virtual ASYNC killNote( const TQString& noteId, bool force );
    virtual TQMap<TQString,TQString> notes();
    virtual ASYNC setName( const TQString& noteId, const TQString& newName );
    virtual ASYNC setText( const TQString& noteId, const TQString& newText );
    virtual TQString name( const TQString& noteId );
    virtual TQString text( const TQString& noteId );
    virtual ASYNC sync( const TQString& app );
    virtual bool isNew( const TQString& app, const TQString& noteId );
    virtual bool isModified( const TQString& app, const TQString& noteId );
protected:
    KNotesIface_stub() : DCOPStub( never_use ) {}
};


#endif
