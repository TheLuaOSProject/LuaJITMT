-- Every return value is compared through warm entry, native reuse and exits.
local function run(count,ninvariant,rotate,nint,frame,copyspill,fma)
  local names,init,body,inputs,args={},{},{},{},{31,.5,1}
  for i=1,ninvariant do inputs[i]='b'..i;args[#args+1]=1+i/1000 end
  for i=1,count do
    names[#names+1]='x'..i;init[#init+1]=('a+%.4f'):format(i/1000)
    if i>rotate then
      if ninvariant>0 then
        body[#body+1]=('x%d=x%d*b%d+b%d;'):format(i,i,(i-1)%ninvariant+1,i%ninvariant+1)
      else
        body[#body+1]=('x%d=x%d*%s+.125;'):format(i,i,fma and '.5' or '1.0001')
      end
    end
  end
  if rotate>0 then table.insert(body,1,'x1,x2=x2,x1;') end
  for i=1,nint do
    names[#names+1]='z'..i;init[#init+1]='k+'..i
    body[#body+1]=('z%d=z%d+%d;'):format(i,i,i)
  end
  local source='return function(n,a,k'..(#inputs>0 and ',' or '')..table.concat(inputs,',')..') local '..
    table.concat(names,',')..'='..table.concat(init,',')..';local i=0;while i<n do '..
    table.concat(body,' ')..'i=i+1 end return '..table.concat(names,',')..' end'
  local f=assert(loadstring(source,'numeric-spill-pressure'))()
  local baseline=assert(loadstring(source,'numeric-spill-baseline'))()
  jit.off(baseline)
  local expected={baseline(unpack(args))}
  local function compare(t)
    assert(#t==#expected)
    for i=1,#t do assert(t[i]==expected[i],('result %d: %.17g ~= %.17g'):format(i,t[i],expected[i])) end
  end
  jit.flush();jit.opt.start('hotloop=1','hotexit=1000','maxtrace=1',fma and 'fma' or '-fma')
  print('CASE',count,ninvariant,rotate,nint,frame,fma and 'fma' or 'separate')
  compare({f(unpack(args))})
  probe_reset();compare({f(unpack(args))});probe_check(frame,copyspill,fma)
  probe_lifecycle(f,expected,unpack(args))
  args[1]=8.5
  probe_changed(f,{baseline(unpack(args))},unpack(args))
  if nint>0 then
    args[1]=31;args[3]=2147483547
    probe_changed(f,{baseline(unpack(args))},unpack(args))
  end
  probe_flush()
end
jit.off(run,true)
run(24,8,2,0,0,false,false)
run(30,4,2,0,16,false,false)
run(31,8,2,0,64,false,false)
for _,count in ipairs({32,36,40,48,60}) do run(count,0,0,0,-1,count==36,false) end
run(36,0,0,0,-1,true,true)
run(36,0,0,26,-1,true,false)
