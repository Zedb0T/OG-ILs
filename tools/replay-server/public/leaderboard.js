'use strict';
const $ = id => document.getElementById(id);
const state = {source:'speedrun',group:'all',view:'points',id:'',offset:0,next:null,total:0};
let generation=0, loading=false, activeController=null, displayedContext='';
const limit=50;
function syncURL(){const q=new URLSearchParams({source:state.source,group:state.group,view:state.view});if(state.id)q.set('id',state.id);if(state.offset)q.set('offset',state.offset);history.replaceState(null,'','/?'+q);}
function node(tag,text,cls){const el=document.createElement(tag);if(text!==undefined)el.textContent=text;if(cls)el.className=cls;return el;}
function timeLabel(seconds){if(seconds===null)return '—';const ms=Math.round(seconds*1000),mins=Math.floor(ms/60000);return mins?`${mins}:${((ms%60000)/1000).toFixed(3).padStart(6,'0')}`:(ms/1000).toFixed(3)+'s';}
function openView(view,id=''){state.view=view;state.id=id;state.offset=0;load();}
function rowButton(text,view,id,cls){const b=node('button',text,cls);b.onclick=()=>openView(view,id);return b;}
function runLink(row){if(!row.run_url)return node('span','—');const a=node('a',state.source==='ghosts'?'Replay ↓':'View run ↗','run-link');a.href=row.run_url;if(state.source==='speedrun'){a.target='_blank';a.rel='noopener noreferrer';}return a;}
async function fetchJSON(url,controller,cache){
  let timedOut=false;const timer=setTimeout(()=>{timedOut=true;controller.abort();},20000);
  try{const response=await fetch(url,{signal:controller.signal,cache});const data=await response.json();if(!response.ok)throw Error(data.error||'Could not load this leaderboard.');return data;}
  catch(error){if(timedOut)throw Error('The server took too long to respond. Please try Refresh.');throw error;}
  finally{clearTimeout(timer);}
}
function render(data){
  $('summary').hidden=false;$('stat-players').textContent=data.total_players;$('stat-missions').textContent=data.ranked_missions;
  const configs={points:['Points standings','Your best result on each mission, added together.',['Rank','Player','Points','Missions','WRs','Tied WRs']],missions:['Mission leaderboards','Select a mission to see its times and points.',['#','Mission','Players','World record']],mission:[data.mission?.label,'One personal best per player. '+(data.mission?.variants?.join(' · ')||''),['Place','Player','Time','Points','Run']],player:[data.player?.display_name,`${data.player?.points??0} points · Rank ${data.player?.rank??'—'} · ${data.player?.mission_count??0} missions`,['Place','Mission','Time','Points','Run']]};
  const [title,description,headings]=configs[state.view];$('board-title').textContent=title;$('table-caption').textContent=title;$('board-description').textContent=description;
  $('back').hidden=!['mission','player'].includes(state.view);$('view-points').setAttribute('aria-pressed',String(['points','player'].includes(state.view)));$('view-missions').setAttribute('aria-pressed',String(['missions','mission'].includes(state.view)));
  $('table-head').replaceChildren();const tr=node('tr');headings.forEach(h=>{const th=node('th',h);th.scope='col';tr.append(th)});$('table-head').append(tr);$('table-body').replaceChildren();
  data.items.forEach((item,index)=>{
    const row=node('tr');let cells=[];
    if(state.view==='points')cells=[node('span',String(item.rank).padStart(2,'0'),item.rank<=3?'rank top-rank':'rank'),rowButton(item.display_name,'player',item.player_id,'player-link'),node('span',item.points.toLocaleString(),'points'),item.mission_count,item.untied_wr_count,item.tied_wr_count];
    else if(state.view==='missions')cells=[node('span',state.offset+index+1,'rank'),rowButton(item.label,'mission',item.mission_id,'mission-link'),item.player_count,timeLabel(item.wr_seconds)];
    else cells=[node('span',item.place,item.place<=3?'rank top-rank':'rank'),state.view==='mission'?rowButton(item.display_name,'player',item.player_id,'player-link'):rowButton(item.mission_label,'mission',item.mission_id,'mission-link'),timeLabel(item.duration_seconds),node('span',item.points,'points'),runLink(item)];
    cells.forEach(cell=>{const td=node('td');td.append(cell instanceof Node?cell:document.createTextNode(String(cell)));row.append(td)});$('table-body').append(row);
  });
  state.next=data.next_offset;state.total=data.total;$('row-count').textContent=data.total+' '+(state.view==='missions'?'missions':state.view==='player'?'results':'players');
  $('table-wrap').hidden=data.items.length===0;$('status').hidden=data.items.length>0;$('status').className='';$('status').textContent=state.offset?'No more results on this page.':'No ranked runs yet for this selection.';
  $('page-label').textContent=data.total?`${Math.min(state.offset+1,data.total)}–${Math.min(state.offset+limit,data.total)} of ${data.total}`:'0 results';
  $('previous').disabled=state.offset===0;$('next').disabled=state.next===null;
  if(state.source==='ghosts')$('freshness').textContent='Ghost standings update after submissions and name changes. Last change: '+new Date(data.updated_at).toLocaleString()+'.';
}
async function load(force=false){
  const current=++generation;if(activeController)activeController.abort();activeController=new AbortController();const controller=activeController;
  loading=true;$('refresh').disabled=true;$('status').hidden=false;$('status').className='';$('status').textContent='Loading the leaderboard…';document.querySelector('.board').setAttribute('aria-busy','true');
  const context=[state.source,state.group,state.view,state.id,state.offset].join(':');
  if(context!==displayedContext){
    displayedContext=context;$('table-wrap').hidden=true;$('summary').hidden=true;$('freshness').textContent='';$('row-count').textContent='';$('page-label').textContent='';
    $('board-title').textContent={points:'Points standings',missions:'Mission leaderboards',mission:'Mission results',player:'Player results'}[state.view];$('board-description').textContent='';
  }
  $('previous').disabled=true;$('next').disabled=true;
  $('back').hidden=!['mission','player'].includes(state.view);$('view-points').setAttribute('aria-pressed',String(['points','player'].includes(state.view)));$('view-missions').setAttribute('aria-pressed',String(['missions','mission'].includes(state.view)));
  $('source').value=state.source;$('group').value=state.group;$('source-note').textContent=state.source==='speedrun'?'Speedrun.com · Verified Any% runs · Default subcategories':'Uploaded ghosts · Community submissions, not independently verified';syncURL();
  const params=new URLSearchParams({source:state.source,game:'jak3',group:state.group,offset:state.offset,limit});
  let route=state.view==='points'?'leaderboards/points':state.view==='missions'?'missions':state.view==='mission'?`missions/${encodeURIComponent(state.id)}/leaderboard`:`players/${encodeURIComponent(state.id)}`;
  try{
    const data=await fetchJSON('/api/v1/'+route+'?'+params,controller,force?'no-cache':'default');
    if(current!==generation)return;render(data);
  }catch(error){if(current!==generation||error.name==='AbortError')return;$('table-wrap').hidden=true;$('summary').hidden=true;$('row-count').textContent='';$('status').hidden=false;$('status').className='error';$('status').textContent=error.message;$('previous').disabled=state.offset===0;$('next').disabled=true;$('page-label').textContent='';}
  finally{if(current===generation){loading=false;$('refresh').disabled=false;document.querySelector('.board').setAttribute('aria-busy','false');}}
  if(state.source==='speedrun'&&current===generation){try{const status=await fetchJSON('/api/v1/status?source=speedrun',controller,'no-store');if(current!==generation)return;
    $('freshness').textContent=!status.available?`Building Speedrun.com cache: ${status.completed_missions}/${status.total_missions||'…'} missions. ${status.message||'This first refresh may take a few minutes.'}`:(status.stale?'Showing last-good cached results. ':'Cached results · ')+new Date(status.updated_at).toLocaleString()+(status.refreshing?' · Refreshing in the background.':status.message?' · Refresh delayed; retry scheduled.':' · Refreshes hourly.');
  }catch(error){if(current===generation&&error.name!=='AbortError')$('freshness').textContent='Cache status unavailable. Use Refresh to try again.';}}
}
$('source').onchange=()=>{state.source=$('source').value;state.view='points';state.id='';state.offset=0;load();};
$('group').onchange=()=>{state.group=$('group').value;state.offset=0;if(['mission','player'].includes(state.view)){state.view='points';state.id='';}load();};
$('view-points').onclick=()=>openView('points');$('view-missions').onclick=()=>openView('missions');$('back').onclick=()=>openView(state.view==='mission'?'missions':'points');
$('previous').onclick=()=>{state.offset=Math.max(0,state.offset-limit);load();};$('next').onclick=()=>{if(state.next!==null){state.offset=state.next;load();}};$('refresh').onclick=()=>load(true);
const initial=new URLSearchParams(location.search);if(['speedrun','ghosts'].includes(initial.get('source')))state.source=initial.get('source');if(['all','main','orb','side'].includes(initial.get('group')))state.group=initial.get('group');
if(['points','missions','mission','player'].includes(initial.get('view')))state.view=initial.get('view');if(/^[A-Za-z0-9_-]{1,96}$/.test(initial.get('id')||''))state.id=initial.get('id');if(['mission','player'].includes(state.view)&&!state.id)state.view='points';
const initialOffset=Number(initial.get('offset'));if(Number.isInteger(initialOffset)&&initialOffset>=0&&initialOffset<=1000000)state.offset=initialOffset;
setInterval(()=>{if(!document.hidden&&!loading)load();},30000);document.addEventListener('visibilitychange',()=>{if(!document.hidden&&!loading)load();});load();
