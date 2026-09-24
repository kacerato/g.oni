#!/usr/bin/env python3
# P4.5.2 — matriz cruzada de binding JNI: EditorJni.kt ↔ EditorJni.cpp
# Saída: tabela markdown + lista de símbolos C esperados (p/ dlsym) +
# mismatches (faltantes/extras/assinaturas divergentes).
import re
import sys

# Raiz do repo = dois níveis acima deste script (repo/scripts/…).
REPO = __import__("pathlib").Path(__file__).resolve().parent.parent
KOTLIN = str(REPO / "android/app/src/main/java/com/goni/runtime/EditorJni.kt")
CPP = str(REPO / "android/app/src/main/cpp/EditorJni.cpp")

PARAM_MAP = {
    "Long": "jlong",
    "Boolean": "jboolean",
    "Int": "jint",
    "Float": "jfloat",
    "String": "jstring",
    "String?": "jstring",
    "Surface": "jobject",
    "Surface?": "jobject",
}
RETURN_MAP = dict(PARAM_MAP)
RETURN_MAP.update({
    "String?": "jstring",
    "FloatArray?": "jfloatArray",
    "Unit": "void",
})


def parse_kotlin(path):
    src = open(path, encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)  # remove docstrings
    funs = []
    for m in re.finditer(
        r"external\s+fun\s+(\w+)\s*\(([^)]*)\)\s*(?::\s*([\w?<>]+))?", src
    ):
        name, params, ret = m.group(1), m.group(2), m.group(3) or "Unit"
        plist = []
        for p in params.split(","):
            p = p.strip()
            if not p:
                continue
            pname, ptype = p.rsplit(":", 1)
            plist.append((pname.strip(), ptype.strip()))
        funs.append((name, plist, ret.strip()))
    return funs


def parse_cpp(path):
    src = open(path, encoding="utf-8").read()
    jni_defs = {}
    for m in re.finditer(
        r"JNIEXPORT\s+(\w+)\s+JNICALL\s+Java_com_goni_runtime_EditorJni_(\w+)"
        r"\s*\(([^)]*)\)",
        src,
        flags=re.S,
    ):
        ret, name, params = m.group(1), m.group(2), m.group(3)
        defs = []
        for p in params.split(","):
            p = re.sub(r"/\*.*?\*/", "", p)  # remove /*thiz*/ etc.
            p = " ".join(p.split())
            if not p:
                continue
            toks = p.split(" ")
            # remove o NOME do parâmetro (último token identificador):
            # "jlong handle" -> "jlong"; "const jfloat* values" -> "const jfloat*"
            if len(toks) > 1 and re.fullmatch(r"[A-Za-z_]\w*", toks[-1]):
                p = " ".join(toks[:-1])
            defs.append(p)
        if (
            len(defs) < 2
            or not defs[0].startswith("JNIEnv*")
            or not defs[1].startswith("jobject")
        ):
            print(f"ERRO: {name}: cabeçalho JNI inesperado: {defs}", file=sys.stderr)
            sys.exit(1)
        jni_defs[name] = (ret, defs[2:])
    return jni_defs


def main():
    kotlin = parse_kotlin(KOTLIN)
    cpp = parse_cpp(CPP)

    k_names = [k[0] for k in kotlin]
    dup = {n for n in k_names if k_names.count(n) > 1}
    if dup:
        print(f"ERRO: funs duplicadas no Kotlin: {dup}", file=sys.stderr)
        sys.exit(1)

    problems = []
    rows = []
    for name, params, ret in kotlin:
        if name not in cpp:
            problems.append(f"FALTANTE no .cpp: {name} ({ret})")
            rows.append((name, ret, params, "—", "FALTANTE"))
            continue
        cret, cparams = cpp[name]
        expected_c = [PARAM_MAP.get(t, f"?{t}") for _, t in params]
        exp_ret = RETURN_MAP.get(ret, f"?{ret}")
        ok = exp_ret == cret and expected_c == cparams
        if exp_ret != cret:
            problems.append(
                f"RETORNO divergente: {name}: Kotlin {ret} → esperado {exp_ret}, cpp tem {cret}"
            )
        if expected_c != cparams:
            problems.append(
                f"PARÂMETROS divergentes: {name}: esperado ({', '.join(expected_c)}), cpp tem ({', '.join(cparams)})"
            )
        rows.append((name, ret, params, cret, "ok" if ok else "DIVERGE"))
    for name in cpp:
        if name not in k_names:
            problems.append(f"EXTRA no .cpp (sem external fun no Kotlin): {name}")

    prefix = "Java_com_goni_runtime_EditorJni_"
    symbols = sorted(prefix + name for name, _, _ in kotlin)

    print(f"Kotlin external funs: {len(kotlin)}")
    print(f"Definições JNI no cpp: {len(cpp)}")
    print()
    print("| # | Kotlin | retorno | parâmetros | JNI cpp | estado |")
    print("|---|--------|---------|------------|---------|--------|")
    for i, (name, ret, params, cret, st) in enumerate(rows, 1):
        pstr = ", ".join(f"{n}: {t}" for n, t in params) or "—"
        print(f"| {i} | `{name}` | `{ret}` | `{pstr}` | `{cret}` | {st} |")
    print()
    if problems:
        print("## PROBLEMAS")
        for p in problems:
            print(f"- {p}")
    else:
        print("## 1:1 VERIFICADO — nenhuma divergência de nome/assinatura")
    print()
    print("## Símbolos C esperados no .so (contrato dlsym)")
    print("```cpp")
    print("static constexpr const char* kExpectedJniSymbols[] = {")
    for s in symbols:
        print(f'    "{s}",')
    print("};")
    print("```")
    if problems:
        sys.exit(2)


if __name__ == "__main__":
    main()
