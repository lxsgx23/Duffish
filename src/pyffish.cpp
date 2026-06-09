/*
  Based on Jean-Francois Romang work
  https://github.com/jromang/Stockfish/blob/pyfish/src/pyfish.cpp
*/

#include <Python.h>
#include <cstring>

#include "misc.h"
#include "types.h"
#include "bitboard.h"
#include "evaluate.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "piece.h"
#include "variant.h"
#include "apiutil.h"

using namespace Stockfish;

static PyObject* PyFFishError;

const Variant* get_variant_or_error(const char* variant) {
    std::string name = (!variant || strlen(variant) == 0 || strcmp(variant, "standard") == 0 || strcmp(variant, "Standard") == 0)
                     ? "xiangqi"
                     : std::string(variant);
    if (name != "xiangqi")
    {
        PyErr_SetString(PyExc_ValueError, (std::string("Unsupported variant '") + name + "'. Duffish supports only xiangqi.").c_str());
        return nullptr;
    }
    return xiangqi_variant();
}

bool buildPosition(Position& pos, StateListPtr& states, const char *variant, const char *fen, PyObject *moveList) {
    states = StateListPtr(new std::deque<StateInfo>(1)); // Drop old and create a new one

    const Variant* v = get_variant_or_error(variant);
    if (!v)
        return false;
    UCI::init_variant(v);
    if (strcmp(fen, "startpos") == 0)
        fen = v->startFen.c_str();
    pos.set(v, std::string(fen), false, &states->back(), Threads.main());

    // parse move list
    int numMoves = PyList_Size(moveList);
    for (int i = 0; i < numMoves ; i++)
    {
        PyObject *MoveStr = PyUnicode_AsEncodedString( PyList_GetItem(moveList, i), "UTF-8", "strict");
        std::string moveStr(PyBytes_AS_STRING(MoveStr));
        Py_XDECREF(MoveStr);
        Move m;
        if ((m = UCI::to_move(pos, moveStr)) != MOVE_NONE)
        {
            // do the move
            states->emplace_back();
            pos.do_move(m, states->back());
        }
        else
        {
            PyErr_SetString(PyExc_ValueError, (std::string("Invalid move '") + moveStr + "'").c_str());
            return false;
        }
    }
    return true;
}

extern "C" PyObject* pyffish_version(PyObject* self) {
    return Py_BuildValue("(iii)", 0, 0, 88);
}

extern "C" PyObject* pyffish_info(PyObject* self) {
    return Py_BuildValue("s", engine_info().c_str());
}

extern "C" PyObject* pyffish_variants(PyObject* self, PyObject *args) {
    PyObject* varList = PyList_New(0);

    for (std::string v : variants.get_keys())
    {
        PyObject* variant = Py_BuildValue("s", v.c_str());
        PyList_Append(varList, variant);
        Py_XDECREF(variant);
    }

    PyObject* Result = Py_BuildValue("O", varList);
    Py_XDECREF(varList);
    return Result;
}

// INPUT option name, option value
extern "C" PyObject* pyffish_setOption(PyObject* self, PyObject *args) {
    const char *name;
    PyObject *valueObj;
    if (!PyArg_ParseTuple(args, "sO", &name, &valueObj)) return NULL;

    if (Options.count(name))
    {
        PyObject *Value = PyUnicode_AsEncodedString( PyObject_Str(valueObj), "UTF-8", "strict");
        Options[name] = std::string(PyBytes_AS_STRING(Value));
        Py_XDECREF(Value);
    }
    else
    {
        PyErr_SetString(PyExc_ValueError, (std::string("No such option ") + name + "'").c_str());
        return NULL;
    }
    Py_RETURN_NONE;
}

// INPUT variant
extern "C" PyObject* pyffish_startFen(PyObject* self, PyObject *args) {
    const char *variant;

    if (!PyArg_ParseTuple(args, "s", &variant)) {
        return NULL;
    }

    const Variant* v = get_variant_or_error(variant);
    if (!v)
        return NULL;
    return Py_BuildValue("s", v->startFen.c_str());
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_legalMoves(PyObject* self, PyObject *args) {
    PyObject* legalMoves = PyList_New(0), *moveList;
    Position pos;
    const char *fen, *variant;

    if (!PyArg_ParseTuple(args, "ssO!", &variant, &fen, &PyList_Type, &moveList)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    for (const auto& m : MoveList<LEGAL>(pos))
    {
        PyObject *moveStr;
        moveStr = Py_BuildValue("s", UCI::move(pos, m).c_str());
        PyList_Append(legalMoves, moveStr);
        Py_XDECREF(moveStr);
    }

    PyObject *Result = Py_BuildValue("O", legalMoves);  
    Py_XDECREF(legalMoves);
    return Result;
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_getFEN(PyObject* self, PyObject *args) {
    PyObject *moveList;
    Position pos;
    const char *fen, *variant;

    int sfen = false, showPromoted = false, countStarted = 0;
    if (!PyArg_ParseTuple(args, "ssO!|ppi", &variant, &fen, &PyList_Type, &moveList, &sfen, &showPromoted, &countStarted)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    return Py_BuildValue("s", pos.fen(sfen, showPromoted, countStarted).c_str());
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_givesCheck(PyObject* self, PyObject *args) {
    PyObject *moveList;
    Position pos;
    const char *fen, *variant;
    if (!PyArg_ParseTuple(args, "ssO!", &variant, &fen,  &PyList_Type, &moveList)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    return Py_BuildValue("O", Stockfish::checked(pos) ? Py_True : Py_False);
}

// INPUT variant, fen, move list, move
extern "C" PyObject* pyffish_isCapture(PyObject* self, PyObject *args) {
    Position pos;
    PyObject *moveList;
    const char *variant, *fen, *move;
    if (!PyArg_ParseTuple(args, "ssO!s", &variant, &fen, &PyList_Type, &moveList, &move)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    std::string moveStr = move;

    return Py_BuildValue("O", pos.capture(UCI::to_move(pos, moveStr)) ? Py_True : Py_False);
}

// INPUT variant, fen, move list
// should only be called when the move list is empty
extern "C" PyObject* pyffish_gameResult(PyObject* self, PyObject *args) {
    PyObject *moveList;
    Position pos;
    const char *fen, *variant;
    bool gameEnd;
    Value result;
    if (!PyArg_ParseTuple(args, "ssO!", &variant, &fen, &PyList_Type, &moveList)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    assert(!MoveList<LEGAL>(pos).size());
    gameEnd = pos.is_immediate_game_end(result);
    if (!gameEnd)
        result = pos.checkers() ? pos.checkmate_value() : pos.stalemate_value();

    return Py_BuildValue("i", result);
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_isImmediateGameEnd(PyObject* self, PyObject *args) {
    PyObject *moveList;
    Position pos;
    const char *fen, *variant;
    bool gameEnd;
    Value result;
    if (!PyArg_ParseTuple(args, "ssO!", &variant, &fen, &PyList_Type, &moveList)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    gameEnd = pos.is_immediate_game_end(result);
    return Py_BuildValue("(Oi)", gameEnd ? Py_True : Py_False, result);
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_isOptionalGameEnd(PyObject* self, PyObject *args) {
    PyObject *moveList;
    Position pos;
    const char *fen, *variant;
    bool gameEnd;
    Value result;
    int countStarted = 0;
    if (!PyArg_ParseTuple(args, "ssO!|i", &variant, &fen, &PyList_Type, &moveList, &countStarted)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;
    gameEnd = pos.is_optional_game_end(result, 0, countStarted);
    return Py_BuildValue("(Oi)", gameEnd ? Py_True : Py_False, result);
}

// INPUT variant, fen, move list
extern "C" PyObject* pyffish_hasInsufficientMaterial(PyObject* self, PyObject *args) {
    PyObject *moveList;
    Position pos;
    const char *fen, *variant;
    if (!PyArg_ParseTuple(args, "ssO!", &variant, &fen, &PyList_Type, &moveList)) {
        return NULL;
    }

    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;

    bool wInsufficient = has_insufficient_material(WHITE, pos);
    bool bInsufficient = has_insufficient_material(BLACK, pos);

    return Py_BuildValue("(OO)", wInsufficient ? Py_True : Py_False, bInsufficient ? Py_True : Py_False);
}

// INPUT variant, fen
extern "C" PyObject* pyffish_validateFen(PyObject* self, PyObject *args) {
    const char *fen, *variant;
    if (!PyArg_ParseTuple(args, "ss", &fen, &variant)) {
        return NULL;
    }

    const Variant* v = get_variant_or_error(variant);
    if (!v)
        return NULL;
    return Py_BuildValue("i", FEN::validate_fen(std::string(fen), v, false));
}

// INPUT variant, fen
extern "C" PyObject* pyffish_getFogFEN(PyObject* self, PyObject *args) {
    PyObject* moveList = PyList_New(0);
    Position pos;
    const char *fen, *variant;

    int sfen = false, showPromoted = false, countStarted = 0;
    if (!PyArg_ParseTuple(args, "ss", &fen, &variant)) {
        return NULL;
    }
    StateListPtr states(new std::deque<StateInfo>(1));
    if (!buildPosition(pos, states, variant, fen, moveList))
        return NULL;

    Py_XDECREF(moveList);
    return Py_BuildValue("s", pos.fen(sfen, showPromoted, countStarted, "-", pos.fog_area()).c_str());
}

static PyMethodDef PyFFishMethods[] = {
    {"version", (PyCFunction)pyffish_version, METH_NOARGS, "Get package version."},
    {"info", (PyCFunction)pyffish_info, METH_NOARGS, "Get Stockfish version info."},
    {"variants", (PyCFunction)pyffish_variants, METH_NOARGS, "Get supported variants."},
    {"set_option", (PyCFunction)pyffish_setOption, METH_VARARGS, "Set UCI option."},
    {"start_fen", (PyCFunction)pyffish_startFen, METH_VARARGS, "Get starting position FEN."},
    {"legal_moves", (PyCFunction)pyffish_legalMoves, METH_VARARGS, "Get legal moves from given FEN and movelist."},
    {"get_fen", (PyCFunction)pyffish_getFEN, METH_VARARGS, "Get resulting FEN from given FEN and movelist."},
    {"gives_check", (PyCFunction)pyffish_givesCheck, METH_VARARGS, "Get check status from given FEN and movelist."},
    {"is_capture", (PyCFunction)pyffish_isCapture, METH_VARARGS, "Get whether given move is a capture from given FEN and movelist."},
    {"game_result", (PyCFunction)pyffish_gameResult, METH_VARARGS, "Get result from given FEN, considering variant end, checkmate, and stalemate."},
    {"is_immediate_game_end", (PyCFunction)pyffish_isImmediateGameEnd, METH_VARARGS, "Get result from given FEN if variant rules ends the game."},
    {"is_optional_game_end", (PyCFunction)pyffish_isOptionalGameEnd, METH_VARARGS, "Get result from given FEN it rules enable game end by player."},
    {"has_insufficient_material", (PyCFunction)pyffish_hasInsufficientMaterial, METH_VARARGS, "Checks for insufficient material."},
    {"validate_fen", (PyCFunction)pyffish_validateFen, METH_VARARGS, "Validate an input FEN."},
    {"get_fog_fen", (PyCFunction)pyffish_getFogFEN, METH_VARARGS, "Get Fog of War FEN from given FEN."},
    {NULL, NULL, 0, NULL},  // sentinel
};

static PyModuleDef pyffishmodule = {
    PyModuleDef_HEAD_INIT,
    "pyffish",
    "Fairy-Stockfish extension module.",
    -1,
    PyFFishMethods,
};

PyMODINIT_FUNC PyInit_pyffish() {
    PyObject* module;

    module = PyModule_Create(&pyffishmodule);
    if (module == NULL) {
        return NULL;
    }
    PyFFishError = PyErr_NewException("pyffish.error", NULL, NULL);
    Py_INCREF(PyFFishError);
    PyModule_AddObject(module, "error", PyFFishError);

    // values
    PyModule_AddObject(module, "VALUE_MATE", PyLong_FromLong(VALUE_MATE));
    PyModule_AddObject(module, "VALUE_DRAW", PyLong_FromLong(VALUE_DRAW));

    // validation
    PyModule_AddObject(module, "FEN_OK", PyLong_FromLong(FEN::FEN_OK));

    // initialize stockfish
    pieceMap.init();
    variants.init();
    UCI::init(Options);
    PSQT::init(xiangqi_variant());
    Bitboards::init();
    Position::init();
    Search::init();
    Threads.set(Options["Threads"]);
    Search::clear(); // After threads are up

    return module;
};
