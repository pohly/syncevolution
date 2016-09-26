/****************************************************************************
**
** DCOP Stub Implementation created by dcopidl2cpp from KNotesIface.kidl
**
** WARNING! All changes made in this file will be lost!
**
** $Id: KNotesIface_stub.cpp,v 1.2 2016/09/01 10:40:05 emanoil Exp $
**
*****************************************************************************/

#include "KNotesIface_stub.h"
#include <dcopclient.h>

#include <tqdatastream.h>


KNotesIface_stub::KNotesIface_stub( const TQCString& app, const TQCString& obj )
  : DCOPStub( app, obj )
{
}

KNotesIface_stub::KNotesIface_stub( DCOPClient* client, const TQCString& app, const TQCString& obj )
  : DCOPStub( client, app, obj )
{
}

KNotesIface_stub::KNotesIface_stub( const DCOPRef& ref )
  : DCOPStub( ref )
{
}

TQString KNotesIface_stub::newNote( const TQString& arg0, const TQString& arg1 )
{
    TQString result;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    arg << arg1;
    if ( dcopClient()->call( app(), obj(), "newNote(TQString,TQString)", data, replyType, replyData ) ) {
	if ( replyType == "TQString" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}

TQString KNotesIface_stub::newNoteFromClipboard( const TQString& arg0 )
{
    TQString result;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    if ( dcopClient()->call( app(), obj(), "newNoteFromClipboard(TQString)", data, replyType, replyData ) ) {
	if ( replyType == "TQString" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}

void KNotesIface_stub::showNote( const TQString& arg0 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    dcopClient()->send( app(), obj(), "showNote(TQString)", data );
    setStatus( CallSucceeded );
}

void KNotesIface_stub::hideNote( const TQString& arg0 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    dcopClient()->send( app(), obj(), "hideNote(TQString)", data );
    setStatus( CallSucceeded );
}

void KNotesIface_stub::killNote( const TQString& arg0 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    dcopClient()->send( app(), obj(), "killNote(TQString)", data );
    setStatus( CallSucceeded );
}

void KNotesIface_stub::killNote( const TQString& arg0, bool arg1 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    arg << arg1;
    dcopClient()->send( app(), obj(), "killNote(TQString,bool)", data );
    setStatus( CallSucceeded );
}

TQMap<TQString,TQString> KNotesIface_stub::notes()
{
    TQMap<TQString,TQString> result;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    if ( dcopClient()->call( app(), obj(), "notes()", data, replyType, replyData ) ) {
	if ( replyType == "TQMap<TQString,TQString>" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}

void KNotesIface_stub::setName( const TQString& arg0, const TQString& arg1 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    arg << arg1;
    dcopClient()->send( app(), obj(), "setName(TQString,TQString)", data );
    setStatus( CallSucceeded );
}

void KNotesIface_stub::setText( const TQString& arg0, const TQString& arg1 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    arg << arg1;
    dcopClient()->send( app(), obj(), "setText(TQString,TQString)", data );
    setStatus( CallSucceeded );
}

TQString KNotesIface_stub::name( const TQString& arg0 )
{
    TQString result;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    if ( dcopClient()->call( app(), obj(), "name(TQString)", data, replyType, replyData ) ) {
	if ( replyType == "TQString" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}

TQString KNotesIface_stub::text( const TQString& arg0 )
{
    TQString result;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    if ( dcopClient()->call( app(), obj(), "text(TQString)", data, replyType, replyData ) ) {
	if ( replyType == "TQString" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}

void KNotesIface_stub::sync( const TQString& arg0 )
{
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return;
    }
    TQByteArray data;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    dcopClient()->send( app(), obj(), "sync(TQString)", data );
    setStatus( CallSucceeded );
}

bool KNotesIface_stub::isNew( const TQString& arg0, const TQString& arg1 )
{
    bool result = false;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    arg << arg1;
    if ( dcopClient()->call( app(), obj(), "isNew(TQString,TQString)", data, replyType, replyData ) ) {
	if ( replyType == "bool" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}

bool KNotesIface_stub::isModified( const TQString& arg0, const TQString& arg1 )
{
    bool result = false;
    if ( !dcopClient()  ) {
	setStatus( CallFailed );
	return result;
    }
    TQByteArray data, replyData;
    TQCString replyType;
    TQDataStream arg( data, IO_WriteOnly );
    arg << arg0;
    arg << arg1;
    if ( dcopClient()->call( app(), obj(), "isModified(TQString,TQString)", data, replyType, replyData ) ) {
	if ( replyType == "bool" ) {
	    TQDataStream _reply_stream( replyData, IO_ReadOnly );
	    _reply_stream >> result;
	    setStatus( CallSucceeded );
	} else {
	    callFailed();
	}
    } else { 
	callFailed();
    }
    return result;
}


