/* ****************************************************************************
*
* Copyright (c) Microsoft Corporation. All rights reserved.
*
*
* This file is part of Microsoft R Host.
*
* Microsoft R Host is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 2 of the License, or
* (at your option) any later version.
*
* Microsoft R Host is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with Microsoft R Host.  If not, see <http://www.gnu.org/licenses/>.
*
* ***************************************************************************/

#pragma once
#include "r_api.h"

namespace rhost {
    // Takes type D, and returns the same type, except that any values of
    // of type DevDesc* become of type DevDescT*. E.g. if the input type
    // is a function pointer:
    //    void (*)(int, DevDesc*);
    // it becomes:
    //    void (*)(int, DevDescT*);
    template<class DevDescT, class D>
    class with_versioned_DevDesc {
    private:
        template<class T>
        struct subst {
            typedef T type;
        };

        template<>
        struct subst<DevDesc*> {
            typedef DevDescT* type;
        };

        template<>
        struct subst<const DevDesc*> {
            typedef const DevDescT* type;
        };

        template<class R, class... Args>
        struct subst_args;

        template<class R>
        struct subst_args<R> {
            typedef R(*type)();
        };

        template<class R, class Head, class... Rest>
        struct subst_args<R, Head, Rest...> {
            template<class... RestSub>
            static auto dummy(R(*f)(RestSub...)) -> R(*)(typename subst<Head>::type, RestSub...);

            typedef decltype(dummy(static_cast<typename subst_args<R, Rest...>::type>(nullptr))) type;
        };

        template<class R, class... Args>
        static auto dummy(R(*)(Args...)) -> typename subst_args<R, Args...>::type;

    public:
        typedef decltype(dummy(static_cast<D>(nullptr))) type;
    };

#define RHOST_DEVDESC_MEMBER(x) decltype(::DevDesc::x) x;
#define RHOST_DEVDESC_MEMBER_FUNPTR(x) with_versioned_DevDesc<DevDesc, decltype(::DevDesc::x)>::type x;

    template<int ApiVer>
    struct gd_api;

    template<>
    struct gd_api<10> {
        typedef struct DevDesc* pDevDesc;

        struct DevDesc {
            double left;
            double right;
            double bottom;
            double top;
            double clipLeft;
            double clipRight;
            double clipBottom;
            double clipTop;
            double xCharOffset;
            double yCharOffset;
            double yLineBias;
            double ipr[2];
            double cra[2];
            double gamma;
            Rboolean canClip;
            Rboolean canChangeGamma;
            int canHAdj;
            double startps;
            int startcol;
            int startfill;
            int startlty;
            int startfont;
            double startgamma;
            void *deviceSpecific;
            Rboolean displayListOn;
            Rboolean canGenMouseDown;
            Rboolean canGenMouseMove;
            Rboolean canGenMouseUp;
            Rboolean canGenKeybd;
            Rboolean gettingEvent;
            void(*activate)(const pDevDesc);
            void(*circle)(double x, double y, double r, const pGEcontext gc, pDevDesc dd);
            void(*clip)(double x0, double x1, double y0, double y1, pDevDesc dd);
            void(*close)(pDevDesc dd);
            void(*deactivate)(pDevDesc);
            Rboolean(*locator)(double *x, double *y, pDevDesc dd);
            void(*line)(double x1, double y1, double x2, double y2, const pGEcontext gc, pDevDesc dd);
            void(*metricInfo)(int c, const pGEcontext gc, double* ascent, double* descent, double* width, pDevDesc dd);
            void(*mode)(int mode, pDevDesc dd);
            void(*newPage)(const pGEcontext gc, pDevDesc dd);
            void(*polygon)(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd);
            void(*polyline)(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd);
            void(*rect)(double x0, double y0, double x1, double y1, const pGEcontext gc, pDevDesc dd);
            void(*path)(double *x, double *y, int npoly, int *nper, Rboolean winding, const pGEcontext gc, pDevDesc dd);
            void(*raster)(unsigned int *raster, int w, int h, double x, double y, double width, double height, double rot, Rboolean interpolate, const pGEcontext gc, pDevDesc dd);
            SEXP(*cap)(pDevDesc dd);
            void(*size)(double *left, double *right, double *bottom, double *top, pDevDesc dd);
            double(*strWidth)(const char *str, const pGEcontext gc, pDevDesc dd);
            void(*text)(double x, double y, const char *str, double rot, double hadj, const pGEcontext gc, pDevDesc dd);
            void(*onExit)(pDevDesc dd);
            SEXP(*getEvent)(SEXP, const char *);
            Rboolean(*newFrameConfirm)(pDevDesc dd);
            Rboolean hasTextUTF8;
            void(*textUTF8)(double x, double y, const char *str, double rot, double hadj, const pGEcontext gc, pDevDesc dd);
            double(*strWidthUTF8)(const char *str, const pGEcontext gc, pDevDesc dd);
            Rboolean wantSymbolUTF8;
            Rboolean useRotatedTextInContour;
            SEXP eventEnv;
            void(*eventHelper)(pDevDesc dd, int code);
            int(*holdflush)(pDevDesc dd, int level);
            int haveTransparency;
            int haveTransparentBg;
            int haveRaster;
            int haveCapture, haveLocator;
            char reserved[64];
        };
    };

    template<>
    struct gd_api<11> : gd_api<10> {
    };

    template<>
    struct gd_api<12> {
        typedef struct DevDesc* pDevDesc;

        struct DevDesc {
            double left;
            double right;
            double bottom;
            double top;
            double clipLeft;
            double clipRight;
            double clipBottom;
            double clipTop;
            double xCharOffset;
            double yCharOffset;
            double yLineBias;
            double ipr[2];
            double cra[2];
            double gamma;
            Rboolean canClip;
            Rboolean canChangeGamma;
            int canHAdj;
            double startps;
            int startcol;
            int startfill;
            int startlty;
            int startfont;
            double startgamma;
            void *deviceSpecific;
            Rboolean displayListOn;
            Rboolean canGenMouseDown;
            Rboolean canGenMouseMove;
            Rboolean canGenMouseUp;
            Rboolean canGenKeybd;
            Rboolean canGenIdle;
            Rboolean gettingEvent;
            void(*activate)(const pDevDesc);
            void(*circle)(double x, double y, double r, const pGEcontext gc, pDevDesc dd);
            void(*clip)(double x0, double x1, double y0, double y1, pDevDesc dd);
            void(*close)(pDevDesc dd);
            void(*deactivate)(pDevDesc);
            Rboolean(*locator)(double *x, double *y, pDevDesc dd);
            void(*line)(double x1, double y1, double x2, double y2, const pGEcontext gc, pDevDesc dd);
            void(*metricInfo)(int c, const pGEcontext gc, double* ascent, double* descent, double* width, pDevDesc dd);
            void(*mode)(int mode, pDevDesc dd);
            void(*newPage)(const pGEcontext gc, pDevDesc dd);
            void(*polygon)(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd);
            void(*polyline)(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd);
            void(*rect)(double x0, double y0, double x1, double y1, const pGEcontext gc, pDevDesc dd);
            void(*path)(double *x, double *y, int npoly, int *nper, Rboolean winding, const pGEcontext gc, pDevDesc dd);
            void(*raster)(unsigned int *raster, int w, int h, double x, double y, double width, double height, double rot, Rboolean interpolate, const pGEcontext gc, pDevDesc dd);
            SEXP(*cap)(pDevDesc dd);
            void(*size)(double *left, double *right, double *bottom, double *top, pDevDesc dd);
            double(*strWidth)(const char *str, const pGEcontext gc, pDevDesc dd);
            void(*text)(double x, double y, const char *str, double rot, double hadj, const pGEcontext gc, pDevDesc dd);
            void(*onExit)(pDevDesc dd);
            SEXP(*getEvent)(SEXP, const char *);
            Rboolean(*newFrameConfirm)(pDevDesc dd);
            Rboolean hasTextUTF8;
            void(*textUTF8)(double x, double y, const char *str, double rot, double hadj, const pGEcontext gc, pDevDesc dd);
            double(*strWidthUTF8)(const char *str, const pGEcontext gc, pDevDesc dd);
            Rboolean wantSymbolUTF8;
            Rboolean useRotatedTextInContour;
            SEXP eventEnv;
            void(*eventHelper)(pDevDesc dd, int code);
            int(*holdflush)(pDevDesc dd, int level);
            int haveTransparency;
            int haveTransparentBg;
            int haveRaster;
            int haveCapture, haveLocator;
            char reserved[64];
        };
    };

}