include ( ../../mythconfig.mak )
include ( ../../settings.pro )

TEMPLATE = app
CONFIG -= moc qt

trans.path = $${PREFIX}/share/mythtv/i18n/
trans.files = mythflix_fi.qm mythflix_es.qm mythflix_sv.qm mythflix_nl.qm

INSTALLS += trans

SOURCES += dummy.c 
