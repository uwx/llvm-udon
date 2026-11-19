p='lib/Target/UDON/UDONCallGraph.cpp'
s=open(p,encoding='utf-8').read()
if 'PROV DBG' not in s:
    s=s.replace('''    for (auto &[G, P] : NewProv) {
      auto &Slot = PointerProvenance[G];
      for (auto &E : P)
        Slot.push_back(E);
    }
  }''','''    for (auto &[G, P] : NewProv) {
      auto &Slot = PointerProvenance[G];
      for (auto &E : P)
        Slot.push_back(E);
    }
  }
  if (getenv("UDON_DBG_PROV"))
    for (auto &[G, P] : PointerProvenance) {
      errs() << "prov " << G->getName() << " ->";
      for (auto &[PG, PO] : P)
        errs() << " " << PG->getName() << "+" << PO;
      errs() << "\\n";
    }''')
    open(p,'w',encoding='utf-8',newline='').write(s)
print("ok")
