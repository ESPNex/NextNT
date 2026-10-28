/*
 * PROJECT:     ReactOS Applications
 * LICENSE:     LGPL - See COPYING in the top level directory
 * FILE:        base/applications/msconfig_new/nextldrpage.h
 * PURPOSE:     Freeloader configuration page message handler
 * COPYRIGHT:   Copyright 2005-2006 Christoph von Wittich <Christoph@ApiViewer.de>
 *                        2011      Gregor Schneider <Gregor.Schneider@reactos.org>
 *              Copyright 2011-2012 Hermes BELUSCA - MAITO <hermes.belusca@sfr.fr>
 */

#ifndef _NEXTLDRPAGE_H_
#define _NEXTLDRPAGE_H_

extern LPCWSTR lpszNextLdrIni;
extern LPCWSTR lpszBootIni;

INT_PTR CALLBACK NextLdrPageWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

#endif /* _NEXTLDRPAGE_H_ */
