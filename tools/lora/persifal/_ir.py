"""IR node types for Persifal decompiler.

Each node is one asm instruction, classified by mnemonic category.
Operands carry PDB enrichments (symbol, field, type, string) inline.
Fold stages match on node type, not mnemonic strings.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


# ── Mnemonic enum ────────────────────────────────────────────────────

class Kind(str, Enum):
    """Instruction role within the function body."""
    PROLOGUE = "prologue"    # frame setup: push ebp, sub esp, save regs
    EPILOGUE = "epilogue"    # frame teardown: restore regs, add esp, pop ebp, ret
    PAYLOAD  = "payload"     # actual function logic
    INLINE   = "inline"      # inlined ctor/dtor/operator from another function


class Mn(str, Enum):
    """x86 mnemonics. str mixin so Mn.MOV == "mov" for Capstone compat."""
    # moves
    MOV = "mov"
    MOVZX = "movzx"
    MOVSX = "movsx"
    LEA = "lea"
    XCHG = "xchg"
    CMOVA = "cmova"
    CMOVAE = "cmovae"
    CMOVB = "cmovb"
    CMOVBE = "cmovbe"
    CMOVE = "cmove"
    CMOVNE = "cmovne"
    CMOVG = "cmovg"
    CMOVGE = "cmovge"
    CMOVL = "cmovl"
    CMOVLE = "cmovle"
    CMOVS = "cmovs"
    CMOVNS = "cmovns"
    # stack
    PUSH = "push"
    POP = "pop"
    # call/ret
    CALL = "call"
    RET = "ret"
    RETN = "retn"
    # compare
    CMP = "cmp"
    TEST = "test"
    # conditional jumps
    JE = "je"
    JNE = "jne"
    JG = "jg"
    JGE = "jge"
    JL = "jl"
    JLE = "jle"
    JA = "ja"
    JAE = "jae"
    JB = "jb"
    JBE = "jbe"
    JS = "js"
    JNS = "jns"
    JO = "jo"
    JNO = "jno"
    JP = "jp"
    JNP = "jnp"
    # unconditional jump
    JMP = "jmp"
    # arithmetic
    ADD = "add"
    SUB = "sub"
    AND = "and"
    OR = "or"
    XOR = "xor"
    SHL = "shl"
    SHR = "shr"
    SAR = "sar"
    SAL = "sal"
    IMUL = "imul"
    IDIV = "idiv"
    MUL = "mul"
    DIV = "div"
    NEG = "neg"
    NOT = "not"
    INC = "inc"
    DEC = "dec"
    ADC = "adc"
    SBB = "sbb"
    ROL = "rol"
    ROR = "ror"
    RCL = "rcl"
    RCR = "rcr"
    BSF = "bsf"
    BSR = "bsr"
    BT = "bt"
    BTS = "bts"
    BTR = "btr"
    BTC = "btc"
    CDQ = "cdq"
    CWDE = "cwde"
    # SSE moves
    MOVSS = "movss"
    MOVSD = "movsd"
    MOVAPS = "movaps"
    MOVUPS = "movups"
    MOVLPS = "movlps"
    MOVHPS = "movhps"
    MOVD = "movd"
    MOVQ = "movq"
    # SSE arithmetic
    ADDSS = "addss"
    SUBSS = "subss"
    MULSS = "mulss"
    DIVSS = "divss"
    ADDSD = "addsd"
    SUBSD = "subsd"
    MULSD = "mulsd"
    DIVSD = "divsd"
    XORPS = "xorps"
    XORPD = "xorpd"
    ANDPS = "andps"
    ORPS = "orps"
    COMISS = "comiss"
    COMISD = "comisd"
    UCOMISS = "ucomiss"
    UCOMISD = "ucomisd"
    CVTSI2SS = "cvtsi2ss"
    CVTSI2SD = "cvtsi2sd"
    CVTSS2SD = "cvtss2sd"
    CVTSD2SS = "cvtsd2ss"
    CVTTSS2SI = "cvttss2si"
    CVTTSD2SI = "cvttsd2si"
    CVTSS2SI = "cvtss2si"
    CVTSD2SI = "cvtsd2si"
    # FPU
    FLD = "fld"
    FSTP = "fstp"
    FST = "fst"
    FILD = "fild"
    FISTP = "fistp"
    FADD = "fadd"
    FADDP = "faddp"
    FSUB = "fsub"
    FSUBP = "fsubp"
    FMUL = "fmul"
    FMULP = "fmulp"
    FDIV = "fdiv"
    FDIVP = "fdivp"
    FCHS = "fchs"
    FABS = "fabs"
    FCOMP = "fcomp"
    FCOMPP = "fcompp"
    FUCOMP = "fucomp"
    FUCOMPP = "fucompp"
    FSUBR = "fsubr"
    FSUBRP = "fsubrp"
    FDIVR = "fdivr"
    FDIVRP = "fdivrp"
    FSQRT = "fsqrt"
    FSIN = "fsin"
    FCOS = "fcos"
    FPATAN = "fpatan"
    FPTAN = "fptan"
    FLD1 = "fld1"
    FLDLN2 = "fldln2"
    FLDL2E = "fldl2e"
    FLDLG2 = "fldlg2"
    FYL2X = "fyl2x"
    F2XM1 = "f2xm1"
    FSCALE = "fscale"
    FRNDINT = "frndint"
    FIADD = "fiadd"
    FISUB = "fisub"
    FIMUL = "fimul"
    FIDIV = "fidiv"
    FIST = "fist"
    FISTTP = "fisttp"
    FCOMPI = "fcompi"
    FCOMI = "fcomi"
    FUCOMPI = "fucompi"
    FCMOVB = "fcmovb"
    FCMOVNB = "fcmovnb"
    FNSTSW = "fnstsw"
    FNSTCW = "fnstcw"
    FLDCW = "fldcw"
    WAIT = "wait"
    FXCH = "fxch"
    # string ops
    REP_MOVSD = "rep movsd"
    REP_MOVSB = "rep movsb"
    REP_STOSD = "rep stosd"
    REP_STOSB = "rep stosb"
    MOVSB = "movsb"
    MOVSW = "movsw"
    STOSD = "stosd"
    STOSW = "stosw"
    STOSB = "stosb"
    LODSB = "lodsb"
    LODSD = "lodsd"
    SCASB = "scasb"
    SCASD = "scasd"
    REPE_CMPSB = "repe cmpsb"
    REPE_CMPSD = "repe cmpsd"
    # misc
    LAHF = "lahf"
    SAHF = "sahf"
    RDTSC = "rdtsc"
    CLD = "cld"
    STC = "stc"
    CLC = "clc"
    CMC = "cmc"
    SHLD = "shld"
    LEAVE = "leave"
    CPUID = "cpuid"
    # setCC
    SETA = "seta"
    SETAE = "setae"
    SETB = "setb"
    SETBE = "setbe"
    SETE = "sete"
    SETNE = "setne"
    SETG = "setg"
    SETGE = "setge"
    SETL = "setl"
    SETLE = "setle"
    SETS = "sets"
    SETNS = "setns"
    # nop
    NOP = "nop"
    INT3 = "int3"


# str→Mn lookup (built once)
_MN_LOOKUP: dict[str, Mn] = {m.value: m for m in Mn}

def to_mn(s: str) -> Mn | None:
    """Convert Capstone mnemonic string to Mn enum. None if unknown."""
    return _MN_LOOKUP.get(s)


# ── Enriched operand ─────────────────────────────────────────────────

@dataclass
class IROp:
    """One operand of an instruction, with PDB enrichments."""
    # raw asm
    reg: str = ""               # register name (eax, ecx, ...)
    mem_base: str = ""          # memory base register
    mem_index: str = ""         # memory index register
    mem_scale: int = 0          # index scale
    mem_disp: int = 0           # memory displacement
    imm: int = 0                # immediate value
    size: int = 4               # operand size in bytes
    kind: str = ""              # "reg", "mem", "imm"

    # PDB enrichments
    symbol: str = ""            # resolved symbol name
    field: str = ""             # resolved field name
    type: str = ""              # resolved type name
    string: str | None = None   # resolved string literal
    vframe: int | None = None   # vframe offset (stack local)
    sym: object | None = None   # Type from _types.py (runtime type, not string)
    owner: int = 0              # root UDT UID that owns this field (inlinee detection)


# ── Base IR node ─────────────────────────────────────────────────────

@dataclass
class IR:
    """Base: one instruction, enriched."""
    mn: Mn | None = None        # mnemonic enum
    rva: int = 0                # instruction RVA
    lexical: int = -1           # PDB lexical tag (-1 = not provided)
    ops: list[IROp] = field(default_factory=list)
    kind: Kind = Kind.PAYLOAD   # instruction role
    esp_delta: int = 0          # cumulative esp offset at this instruction


# ── Classified subtypes ──────────────────────────────────────────────

@dataclass
class IRMov(IR):
    """mov, movzx, movsx, lea, xchg, cmovCC."""
    pass

@dataclass
class IRPush(IR):
    """push."""
    pass

@dataclass
class IRPop(IR):
    """pop."""
    pass

@dataclass
class IRCall(IR):
    """call."""
    cleanup: int = 0            # callee stack cleanup bytes (from ret N)

@dataclass
class IRCmp(IR):
    """cmp, test."""
    pass

@dataclass
class IRJcc(IR):
    """Conditional jump: je, jne, jg, jge, jl, jle, ja, jae, jb, jbe."""
    target_rva: int = 0

@dataclass
class IRJmp(IR):
    """Unconditional jump: jmp."""
    target_rva: int = 0

@dataclass
class IRRet(IR):
    """ret, retn."""
    cleanup: int = 0            # stack cleanup bytes (ret N)

@dataclass
class IRArith(IR):
    """add, sub, and, or, xor, shl, shr, sar, imul, idiv, neg, not, inc, dec."""
    pass

@dataclass
class IRNop(IR):
    """nop, int3, padding."""
    pass


# ── Mnemonic → subclass mapping ──────────────────────────────────────

_CLS_MAP: dict[Mn, type[IR]] = {}

def _reg(*mns: Mn, cls: type[IR]):
    for m in mns:
        _CLS_MAP[m] = cls

_reg(Mn.MOV, Mn.MOVZX, Mn.MOVSX, Mn.LEA, Mn.XCHG,
     Mn.CMOVA, Mn.CMOVAE, Mn.CMOVB, Mn.CMOVBE,
     Mn.CMOVE, Mn.CMOVNE, Mn.CMOVG, Mn.CMOVGE,
     Mn.CMOVL, Mn.CMOVLE, Mn.CMOVS, Mn.CMOVNS,
     Mn.MOVSS, Mn.MOVSD, Mn.MOVAPS, Mn.MOVUPS,
     Mn.MOVLPS, Mn.MOVHPS, Mn.MOVD, Mn.MOVQ,
     Mn.FLD, Mn.FSTP, Mn.FST, Mn.FILD, Mn.FISTP,
     Mn.FIST, Mn.FISTTP, Mn.FLD1, Mn.FLDLN2, Mn.FLDL2E, Mn.FLDLG2,
     Mn.FCMOVB, Mn.FCMOVNB,
     Mn.SETA, Mn.SETAE, Mn.SETB, Mn.SETBE,
     Mn.SETE, Mn.SETNE, Mn.SETG, Mn.SETGE,
     Mn.SETL, Mn.SETLE, Mn.SETS, Mn.SETNS,
     cls=IRMov)
_reg(Mn.PUSH, cls=IRPush)
_reg(Mn.POP, cls=IRPop)
_reg(Mn.CALL, cls=IRCall)
_reg(Mn.CMP, Mn.TEST, cls=IRCmp)
_reg(Mn.JE, Mn.JNE, Mn.JG, Mn.JGE, Mn.JL, Mn.JLE,
     Mn.JA, Mn.JAE, Mn.JB, Mn.JBE,
     Mn.JS, Mn.JNS, Mn.JO, Mn.JNO, Mn.JP, Mn.JNP, cls=IRJcc)
_reg(Mn.JMP, cls=IRJmp)
_reg(Mn.RET, Mn.RETN, cls=IRRet)
_reg(Mn.ADD, Mn.SUB, Mn.AND, Mn.OR, Mn.XOR,
     Mn.SHL, Mn.SHR, Mn.SAR, Mn.SAL,
     Mn.IMUL, Mn.IDIV, Mn.MUL, Mn.DIV,
     Mn.NEG, Mn.NOT, Mn.INC, Mn.DEC,
     Mn.ADC, Mn.SBB, Mn.ROL, Mn.ROR,
     Mn.RCL, Mn.RCR, Mn.BSF, Mn.BSR,
     Mn.BT, Mn.BTS, Mn.BTR, Mn.BTC,
     Mn.CDQ, Mn.CWDE,
     Mn.ADDSS, Mn.SUBSS, Mn.MULSS, Mn.DIVSS,
     Mn.ADDSD, Mn.SUBSD, Mn.MULSD, Mn.DIVSD,
     Mn.XORPS, Mn.XORPD, Mn.ANDPS, Mn.ORPS,
     Mn.CVTSI2SS, Mn.CVTSI2SD, Mn.CVTSS2SD, Mn.CVTSD2SS,
     Mn.CVTTSS2SI, Mn.CVTTSD2SI, Mn.CVTSS2SI, Mn.CVTSD2SI,
     Mn.FADD, Mn.FADDP, Mn.FSUB, Mn.FSUBP,
     Mn.FMUL, Mn.FMULP, Mn.FDIV, Mn.FDIVP,
     Mn.FSUBR, Mn.FSUBRP, Mn.FDIVR, Mn.FDIVRP,
     Mn.FSQRT, Mn.FSIN, Mn.FCOS, Mn.FPATAN, Mn.FPTAN,
     Mn.FYL2X, Mn.F2XM1, Mn.FSCALE, Mn.FRNDINT,
     Mn.FIADD, Mn.FISUB, Mn.FIMUL, Mn.FIDIV,
     Mn.FCHS, Mn.FABS, Mn.SHLD, cls=IRArith)
_reg(Mn.COMISS, Mn.COMISD, Mn.UCOMISS, Mn.UCOMISD,
     Mn.FCOMP, Mn.FCOMPP, Mn.FUCOMP, Mn.FUCOMPP,
     Mn.FCOMPI, Mn.FCOMI, Mn.FUCOMPI,
     Mn.REPE_CMPSB, Mn.REPE_CMPSD, cls=IRCmp)
_reg(Mn.NOP, Mn.INT3, Mn.WAIT,
     Mn.FNSTSW, Mn.FNSTCW, Mn.FLDCW, Mn.FXCH,
     Mn.REP_MOVSD, Mn.REP_MOVSB, Mn.REP_STOSD, Mn.REP_STOSB,
     Mn.MOVSB, Mn.MOVSW, Mn.STOSD, Mn.STOSW, Mn.STOSB,
     Mn.LODSB, Mn.LODSD, Mn.SCASB, Mn.SCASD,
     Mn.LAHF, Mn.SAHF, Mn.RDTSC, Mn.CLD, Mn.STC, Mn.CLC, Mn.CMC,
     Mn.LEAVE, Mn.CPUID,
     cls=IRNop)


def classify(mn_str: str) -> tuple[type[IR], Mn | None]:
    """Get IR subclass + Mn enum for a Capstone mnemonic string.
    Unknown mnemonic → (IR, None)."""
    mn = to_mn(mn_str)
    if mn is None:
        return IR, None
    return _CLS_MAP.get(mn, IR), mn
