var f;
var upload;
var names = [];
var ul;
var menu;
var uploadMenu;
var newFolderMenu;
window.onload = () => {
    ul = document.querySelector('ul');
    ul.style.setProperty('font-family', 'monospace');
    f = document.getElementById("f");
    upload = document.getElementById("upload");
    for (li of ul.childNodes) {
        names.push(li.innerText);
    }
    uploadMenu = document.createElement('button');
    newFolderMenu = document.createElement('button');
    menu = document.createElement('div');
    menu.style.backgroundColor = 'brown';
    menu.style.position = 'fixed';
    menu.style.borderRadius = '5px';
    menu.style.border = 'solid 5px brown';
    menu.appendChild(uploadMenu);
    menu.appendChild(newFolderMenu);
    menu.style.visibility = 'hidden';
    document.documentElement.appendChild(menu);
    normalMode();
}
function normalMode() {
    uploadMenu.onclick = () => f.click();
    uploadMenu.innerText = 'Upload file';
    uploadMenu.style.backgroundColor = '';
    newFolderMenu.onclick = fnewFolder;
    newFolderMenu.innerText = 'Create folder';
    newFolderMenu.style.backgroundColor = '';
}
function deleteMode() {
    uploadMenu.onclick = fDelete;
    uploadMenu.innerText = 'Delete';
    uploadMenu.style.backgroundColor = 'red';
    newFolderMenu.onclick = fRename;
    newFolderMenu.innerText = "Rename";
    newFolderMenu.style.backgroundColor = 'yellow';
}
function fnewFolder() {
    var folder = prompt("Folder name");
    if (folder) {
        if (names.lastIndexOf(folder) != -1) {
            alert("This Folder alerdy exisits");
        } else {
            /* create Folder */
        }
    }
}
function fDelete() {

}
function fRename() {

}
function fsubmit(e) {
    if (f.files.length == 0) return false;
    for (var i = 0; i < f.files.length; ++i) {
        var overwrite = false;
        if (names.lastIndexOf(f.files[i].name) != -1) {
            if (confirm("over write " + f.files[i].name + "?") == false) {
                continue;
            }
            overwrite = true;
        }
        let x = new XMLHttpRequest();
        x.overwrite = overwrite;
        let processbar = document.createElement("progress");
        upload.appendChild(processbar);
        x.upload.onprogress = (e) => {
            if (e.lengthComputable) {
                processbar.value = e.loaded / e.total;
            }
        };
        x.onloadend = (e) => {
            upload.removeChild(processbar);
        };
        x.onerror = () => {
            alert("Sorry, the file " + x.xfile + ' upload failed\nmaybe try again?');
        };
        x.onreadystatechange = () => {
            if (x.readyState === XMLHttpRequest.DONE && x.status === 200) {
                if (!x.overwrite) {
                    names.push(x.xfile);
                    var n = document.createElement('li');
                    var a = document.createElement('a');
                    a.href = x.xfile;
                    a.innerText = x.xfile;
                    n.appendChild(a);
                    ul.appendChild(n);
                }
            }
        };
        x.open("POST", '/');
        x.setRequestHeader('X-FileName', location.pathname.slice(1) + '%2F' + encodeURIComponent(f.files[i].name));
        x.send(new Blob([f.files[i]]));
        x.xfile = f.files[i].name;
    }
    return false;
};

document.addEventListener('contextmenu', function (event) {
    event.preventDefault();
    menu.style.top = event.y.toString() + 'px';
    menu.style.left = event.x.toString() + 'px';
    menu.style.visibility = 'visible';
    if (event.target.href) {
        deleteMode();
    } else {
        normalMode();       
    }
})
document.addEventListener('click', function (event) {
    menu.style.visibility = 'hidden';
})