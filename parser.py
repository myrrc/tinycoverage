from struct import unpack
import os.path
from io import BytesIO

ENTRY = 0xfefefeaa
INDICES = 0xfefefeab
NAMES = 0xfefefeac

class BinaryParser:
    def __init__(self):
        self.is_le = False

    def read_uint32(self, f):
        data = f.read(4)

        if len(data) != 4:
            raise Exception()

        return unpack("<I" if self.is_le else ">I", data)[0]

    def test_endian(self, f):
        data = f.read(4)

        if unpack(">I", data)[0] == ENTRY:
            self.is_le = False
        elif unpack("<I", data)[0] == ENTRY:
            self.is_le = True
        else:
            raise Exception()

    def read_str(self, f):
        return f.read(self.read_uint32(f) * 4).rstrip(b"\0").decode("utf-8")


class ReportParser(BinaryParser):
    def __init__(self):
        self.tests = []
        self.bb_to_func_idx = {}
        self.func_idx_to_name = {}

    def read(self, file_name):
        with open(file_name, "rb") as f:
            self.read_tests(f)
            self.read_indices(f)
            self.read_names(f)

        return self.tests, self.bb_to_func_idx, self.func_idx_to_name

    def read_tests(self, f):
        self.test_endian(f)

        tests = []

        while True:
            hit_bb = []

            while True:
                token = self.read_uint32(f)

                if token == ENTRY:
                    tests.append(hit_bb)
                    break
                elif token == INDICES:
                    tests.append(hit_bb)
                    self.tests = tests
                    return
                else:
                    hit_bb.append(token)

    def read_indices(self, f):
        i = 0

        while True:
            token = self.read_uint32(f)

            if token == NAMES:
                return
            else:
                self.bb_to_func_idx[i] = token
                i += 1

    def read_names(self, f):
        while True:
            try:
                index = self.read_uint32(f)
                func_name = self.read_str(f)
                self.func_idx_to_name[index] = func_name
            except Exception:
                return

class TcnoParser:
    def __init__(self):
        self.func_to_src = {}
        self.func_to_bb = {}

    def read(self, folder):
        for name in os.listdir(folder):
            with open(os.path.join(folder, name), "r") as f:
                self.read_items(f)

        return self.func_to_src, self.func_to_bb

    def read_items(self, f):
        for line in f.readlines():
            line = line.split()
            file_name, func_name, bb_index = line[:3]
            lineset = sorted(map(int, line[3:]))
            
            bb_index = int(bb_index)

            self.func_to_src[func_name] = file_name

            if func_name not in self.func_to_bb:
                self.func_to_bb[func_name] = {bb_index: lineset}
            else:
                self.func_to_bb[func_name][bb_index] = lineset

tests, bb_to_func_idx, func_idx_to_name = ReportParser().read("report")
func_to_src, func_to_bb = TcnoParser().read("tcno")

bb_idx_to_rel_idx = []

for bb_index, func_idx in enumerate(bb_to_func_idx):
    if bb_index > 0 and bb_to_func_idx[bb_index - 1] == func_idx:
        bb_idx_to_rel_idx.append(bb_idx_to_rel_idx[-1] + 1)
    else:
        bb_idx_to_rel_idx.append(0)

for test in tests:
    print ("Test\n")

    for bb_index in test:
        func_idx = bb_to_func_idx[bb_index]
        func_name = func_idx_to_name[func_idx]
        src = func_to_src[func_name]

        lineset = func_to_bb[func_name][bb_idx_to_rel_idx[bb_index]]


        print(func_name, src, lineset)

