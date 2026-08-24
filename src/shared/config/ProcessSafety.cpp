#include "ProcessSafety.h"

#include <QtCore/QByteArray>
#include <QtGlobal>

void applyProcessSafety()
{
#ifdef Q_OS_WIN
    qputenv("NoDefaultCurrentDirectoryInExePath", QByteArrayLiteral("1"));
#endif
}
