from pathlib import Path
r=Path(__file__).resolve().parent
p=r/'t-jit-cdata-basemt-guards.lua';s=p.read_text();(r/'t-jit-cdata-basemt-guards-v1.lua').write_text(s)
s=s.replace('local exits = {}\n','local exits = {}\nlocal resized = false\n',1)
s=s.replace('  for i = 1, 256 do basemt["guard_resize_" .. i] = nil end\n','  if resized then\n    for i = 1, 256 do basemt["guard_resize_" .. i] = nil end\n    resized = false\n  end\n')
s=s.replace('  elseif mode == "resize" then\n    for i', '  elseif mode == "resize" then\n    resized = true\n    for i')
p.write_text(s)
s=(r/'run-native-guards.py').read_text().replace("name='native-guards-'", "name='native-guards-v2-'").replace("r/'native-guards-results.json'", "r/'native-guards-v2-results.json'")
(r/'run-native-guards-v2.py').write_text(s)
