local t = {}
for i = 1, 1000 do t[i] = {value = i} end
collectgarbage('collect')
for i = 1, 1000 do assert(t[i].value == i) end
print('normal local-completion smoke PASS')
