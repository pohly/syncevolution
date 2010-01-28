PLUGINS=xpcom-dbusservice
LIBRARIES = jscorebus jscorebus-qt

about:
	@echo
	@echo "This is the Browser D-Bus Bridge v1.1"
	@echo "Originally developed by Movial"
	@echo
	@echo "The following build targets are available:"
	@echo "  xpcom              The XPCOM D-Bus service"
	@echo "  jscore             The JavaScriptCore (WebKitGTK) D-Bus library"
	@echo "  jscore-qt          The JavaScriptCore (QtWebKit) D-Bus library"
	@echo "  all                Build all above targets"
	@echo "  install            Install all built targets"
	@echo

build: jscore jscore-qt xpcom

jscore-print:
	@echo
	@echo Building JSCoreBus for WebKitGTK+:
jscore: jscore-print jscorebus-shared

jscore-qt-print:
	@echo
	@echo Building JSCoreBus for QtWebKit:
jscore-qt: jscore-qt-print jscorebus-qt-shared

xpcom-print:
	@echo
	@echo Building XPCOM D-Bus service:
xpcom: xpcom-print xpcom-dbusservice-plugin

include build/project.mk

.PHONY: build all jscorebus jscorebus-qt xpcom-dbusservice

