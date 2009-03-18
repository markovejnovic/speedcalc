// This file is part of the SpeedCrunch project
// Copyright (C) 2009 Helder Correia <helder.pereira.correia@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
// Boston, MA 02110-1301, USA.

#ifndef GUI_APPLICATION_HXX
#define GUI_APPLICATION_HXX

#include <QtGui/QApplication>

#include <memory>

class Application : public QApplication
{
    Q_OBJECT

public:
    Application( int & argc, char * argv[] );
    ~Application();

#if QT_VERSION >= 0x040400

    bool isRunning() const;

signals:
    void raiseRequested();

protected slots:
    void receiveMessage();

#endif // QT_VERSION >= 0x040400

private:
    struct Private;
    const std::auto_ptr<Private> d;

    Application( const Application & );
    Application & operator=( const Application & );
};

#endif // GUI_APPLICATION_HXX

